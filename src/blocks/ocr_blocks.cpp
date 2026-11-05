#include "blocks/ocr_blocks.hpp"

#if (defined(FEATURE_ONNX) && FEATURE_ONNX) \
 || (defined(FEATURE_LLAMA) && FEATURE_LLAMA) \
 || (defined(FEATURE_VLM) && FEATURE_VLM)

#include "core/app_exe_directory.hpp"
#include "core/local_model_catalog.hpp"
#include "core/settings_store.hpp"
#include "core/settings_runtime.hpp"
#include "lib/onnx/OnnxDocLayout.hpp"
#include "lib/onnx/OnnxOcr.hpp"
#include "lib/onnx/OnnxSession.hpp"
#include "llm/llama_vlm_client.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::xblox::blocks {
namespace {

namespace fs = std::filesystem;

using media::strutil::lower_ascii;

std::vector<std::string> strings_from_param(BlockIO& io, const char* name)
{
    std::vector<std::string> out;
    if (const Value* v = io.value(name)) {
        const nlohmann::json j = v->to_json();
        if (j.is_array()) {
            for (const auto& item : j)
                if (item.is_string() && !item.get<std::string>().empty())
                    out.push_back(item.get<std::string>());
            return out;
        }
        if (j.is_string() && !j.get<std::string>().empty())
            out.push_back(j.get<std::string>());
        return out;
    }

    const nlohmann::json& raw = io.json(name);
    if (raw.is_array()) {
        for (const auto& item : raw)
            if (item.is_string() && !item.get<std::string>().empty())
                out.push_back(item.get<std::string>());
    } else if (raw.is_string() && !raw.get<std::string>().empty()) {
        out.push_back(raw.get<std::string>());
    }
    return out;
}

bool file_exists(const std::string& path)
{
    if (path.empty())
        return false;
    std::error_code ec;
    return fs::is_regular_file(media::path::path_from_utf8(path), ec);
}

struct OcrOnnxPreset {
    std::string id;
    std::string label;
    std::string det_model;
    std::string rec_model;
    std::string dict;
};

std::vector<fs::path> ocr_model_roots()
{
    return media::local_models::local_model_roots();
}

std::vector<OcrOnnxPreset> discover_ocr_onnx_presets()
{
    std::vector<OcrOnnxPreset> out;
    std::vector<std::string> seen;
    for (const auto& root : ocr_model_roots()) {
        std::error_code ec;
        if (!fs::is_directory(root, ec))
            continue;

        const fs::path det = root / "en_PP-OCRv3_det_infer.onnx";
        const fs::path rec_v4 = root / "en_PP-OCRv4_rec_infer.onnx";
        const fs::path rec_v3 = root / "en_PP-OCRv3_rec_infer.onnx";
        const fs::path rec = file_exists(media::path::path_to_utf8(rec_v4)) ? rec_v4 : rec_v3;
        const fs::path dict = root / "en_dict.txt";
        if (!file_exists(media::path::path_to_utf8(det)) || !file_exists(media::path::path_to_utf8(rec)) || !file_exists(media::path::path_to_utf8(dict)))
            continue;

        const std::string key = lower_ascii(media::path::path_to_utf8(det) + "|" + media::path::path_to_utf8(rec) + "|" + media::path::path_to_utf8(dict));
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            continue;
        seen.push_back(key);
        out.push_back({
            "paddleocr-en",
            "PaddleOCR English (" + media::path::path_to_utf8(root.filename()) + ")",
            media::path::path_to_utf8(det),
            media::path::path_to_utf8(rec),
            media::path::path_to_utf8(dict),
        });
    }
    return out;
}

bool apply_ocr_onnx_preset(const std::string& preset_id,
                           std::string& det_model,
                           std::string& rec_model,
                           std::string& dict)
{
    if (!det_model.empty() && !rec_model.empty() && !dict.empty())
        return true;
    if (!preset_id.empty() && preset_id != "auto" && preset_id != "paddleocr-en")
        return false;
    for (const auto& preset : discover_ocr_onnx_presets()) {
        if (!preset_id.empty() && preset_id != "auto" && preset.id != preset_id)
            continue;
        if (det_model.empty()) det_model = preset.det_model;
        if (rec_model.empty()) rec_model = preset.rec_model;
        if (dict.empty()) dict = preset.dict;
        return true;
    }
    return false;
}

std::string preferred_ocr_vlm_model()
{
    std::string fallback;
    for (const auto& m : media::local_models::discover_llama_vlm_models(false)) {
        bool has_ocr = false;
        for (const auto& slot : m.slots)
            if (slot == "ocr")
                has_ocr = true;
        if (!has_ocr)
            continue;
        const std::string id = m.id.empty() ? media::path::path_to_utf8(m.model_path) : m.id;
        if (fallback.empty())
            fallback = id;
        const std::string hay = lower_ascii(m.id + " " + m.display_name + " " + media::path::path_to_utf8(m.model_path));
        if (hay.find("paddleocr") != std::string::npos)
            return id;
    }
    return fallback;
}

bool vlm_entry_has_ocr_slot(const media::local_models::LocalModelEntry& entry)
{
    for (const auto& slot : entry.slots)
        if (slot == "ocr")
            return true;
    return false;
}

std::string ocr_vlm_not_found_message(const std::string& requested_model = {})
{
    std::ostringstream os;
    os << "no local OCR VLM available";
    if (!requested_model.empty())
        os << " (requested: \"" << requested_model << "\")";
    os << ". Expected PaddleOCR-VL-1.5.gguf + PaddleOCR-VL-1.5-mmproj.gguf in the same directory, "
          "or a model.json manifest with slots including \"ocr\".";
    os << '\n' << media::local_models::format_discovery_roots_message();

    const auto all_vlm = media::local_models::discover_llama_vlm_models(true);
    bool any_ocr = false;
    bool any_other = false;
    for (const auto& entry : all_vlm) {
        if (vlm_entry_has_ocr_slot(entry)) {
            if (!any_ocr) {
                os << "\nOCR VLM entries:";
                any_ocr = true;
            }
            os << "\n  - " << (entry.id.empty() ? media::path::path_to_utf8(entry.model_path) : entry.id);
            if (!entry.model_path.empty())
                os << " model=" << media::path::path_to_utf8(entry.model_path);
            if (!entry.mmproj_path.empty())
                os << " mmproj=" << media::path::path_to_utf8(entry.mmproj_path);
            if (!entry.valid && !entry.error.empty())
                os << " (invalid: " << entry.error << ')';
            else if (entry.valid)
                os << " (valid)";
        }
    }
    if (!any_ocr)
        os << "\nNo OCR-slot VLM models discovered under those roots.";

    for (const auto& entry : all_vlm) {
        if (vlm_entry_has_ocr_slot(entry))
            continue;
        if (!any_other) {
            os << "\nOther VLM models (no \"ocr\" slot — ignored by ocrText):";
            any_other = true;
        }
        os << "\n  - " << (entry.id.empty() ? media::path::path_to_utf8(entry.model_path) : entry.id);
        if (!entry.valid && !entry.error.empty())
            os << " (invalid: " << entry.error << ')';
    }

    os << "\nPass model= with a catalog id or main GGUF path, or set ocr_model in App Settings.";
    return os.str();
}

void apply_ocr_settings_defaults(std::string& provider, std::string& model)
{
    media::runtime_settings::ChatProviderSettings chat;
    std::string err;
    if (!media::runtime_settings::load_chat_provider(chat, err))
        return;

    if (provider.empty())
        provider = chat.ocr_provider.empty() ? chat.image_recognition_provider
                                             : chat.ocr_provider;
    if (model.empty())
        model = chat.ocr_model.empty() ? chat.image_recognition_model
                                       : chat.ocr_model;
}

nlohmann::json word_json(const media::onnx::OcrWord& w)
{
    return {
        {"x", w.x}, {"y", w.y}, {"w", w.w}, {"h", w.h},
        {"text", w.text},
        {"conf", w.conf},
    };
}

nlohmann::json line_json(const media::onnx::OcrLine& line)
{
    nlohmann::json words = nlohmann::json::array();
    for (const auto& word : line.words)
        words.push_back(word_json(word));
    return {
        {"x", line.x}, {"y", line.y}, {"w", line.w}, {"h", line.h},
        {"text", line.text},
        {"conf", line.conf},
        {"words", std::move(words)},
    };
}

nlohmann::json block_json(const media::onnx::OcrBlock& block)
{
    nlohmann::json lines = nlohmann::json::array();
    for (const auto& line : block.lines)
        lines.push_back(line_json(line));
    return {
        {"x", block.x}, {"y", block.y}, {"w", block.w}, {"h", block.h},
        {"text", block.text},
        {"conf", block.conf},
        {"lines", std::move(lines)},
    };
}

nlohmann::json page_json(const media::onnx::OcrPage& page)
{
    nlohmann::json blocks = nlohmann::json::array();
    for (const auto& block : page.blocks)
        blocks.push_back(block_json(block));
    return {
        {"imageW", page.image_w},
        {"imageH", page.image_h},
        {"text", page.text},
        {"conf", page.conf},
        {"blocks", std::move(blocks)},
    };
}

std::string ocr_onnx_preset_not_found_message(const std::string& preset_id)
{
    std::ostringstream os;
    os << "PaddleOCR ONNX preset not found";
    if (!preset_id.empty() && preset_id != "auto")
        os << " (preset id: \"" << preset_id << "\")";
    os << ". Expected files per root: en_PP-OCRv3_det_infer.onnx, "
          "en_PP-OCRv4_rec_infer.onnx (or en_PP-OCRv3_rec_infer.onnx), en_dict.txt.";
    os << " Probed paths:";
    for (const auto& root : ocr_model_roots()) {
        os << "\n  - " << media::path::path_to_utf8((root / "en_PP-OCRv3_det_infer.onnx"));
        os << "\n  - " << media::path::path_to_utf8((root / "en_PP-OCRv4_rec_infer.onnx"));
        os << "\n  - " << media::path::path_to_utf8((root / "en_PP-OCRv3_rec_infer.onnx"));
        os << "\n  - " << media::path::path_to_utf8((root / "en_dict.txt"));
    }
    os << '\n' << media::local_models::format_discovery_roots_message();
    return os.str();
}

nlohmann::json layout_box_json(const media::onnx::LayoutBox& b)
{
    return {
        {"classId", b.class_id},
        {"label", b.class_name},
        {"score", b.score},
        {"x0", b.x0}, {"y0", b.y0}, {"x1", b.x1}, {"y1", b.y1},
        {"readOrder", b.read_order},
    };
}

bool layout_has_table(const media::onnx::DocLayoutResult& layout)
{
    for (const auto& box : layout.boxes)
        if (box.class_name == "table")
            return true;
    return false;
}

std::string trim_copy(std::string s)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::vector<std::vector<std::string>> parse_table_tokens(const std::string& text)
{
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 4, "<nl>") == 0) {
            if (!row.empty()) {
                rows.push_back(row);
                row.clear();
            }
            i += 4;
            continue;
        }
        if (text.compare(i, 6, "<lcel>") == 0) {
            row.emplace_back();
            i += 6;
            continue;
        }
        if (text.compare(i, 6, "<fcel>") == 0) {
            i += 6;
            const std::size_t next = text.find('<', i);
            row.push_back(trim_copy(text.substr(i, next == std::string::npos ? std::string::npos : next - i)));
            if (next == std::string::npos)
                break;
            i = next;
            continue;
        }
        ++i;
    }
    if (!row.empty())
        rows.push_back(row);
    return rows;
}

std::string md_cell(std::string s)
{
    for (std::size_t pos = 0; (pos = s.find('|', pos)) != std::string::npos; pos += 2)
        s.replace(pos, 1, "\\|");
    for (char& c : s)
        if (c == '\n' || c == '\r')
            c = ' ';
    return s;
}

nlohmann::json receipt_fields_from_rows(const std::vector<std::vector<std::string>>& rows)
{
    nlohmann::json out{
        {"vendor", nullptr},
        {"lineItems", nlohmann::json::array()},
        {"totals", nlohmann::json::object()},
        {"rawRows", rows},
    };
    auto is_num = [](const std::string& s) {
        bool saw_digit = false;
        for (char c : s) {
            if (std::isdigit(static_cast<unsigned char>(c))) saw_digit = true;
            else if (c != '$' && c != ',' && c != '.' && c != ' ') return false;
        }
        return saw_digit;
    };
    for (const auto& row : rows) {
        std::vector<std::string> nonempty;
        for (const auto& cell : row)
            if (!trim_copy(cell).empty())
                nonempty.push_back(trim_copy(cell));
        if (nonempty.empty())
            continue;
        if (out["vendor"].is_null()) {
            out["vendor"] = nonempty.front();
            continue;
        }
        std::string joined;
        for (const auto& cell : nonempty) {
            if (!joined.empty()) joined += " ";
            joined += cell;
        }
        const std::string lower = lower_ascii(joined);
        const auto last_num = std::find_if(nonempty.rbegin(), nonempty.rend(), is_num);
        if (last_num != nonempty.rend() &&
            (lower.find("total") != std::string::npos || lower.find("tax") != std::string::npos ||
             lower.find("subtotal") != std::string::npos || lower.find("shipping") != std::string::npos)) {
            out["totals"][joined.substr(0, joined.rfind(*last_num))] = *last_num;
        } else if (last_num != nonempty.rend() && nonempty.size() >= 2) {
            out["lineItems"].push_back({
                {"description", nonempty.size() >= 2 ? nonempty[nonempty.size() - 2] : std::string{}},
                {"amount", *last_num},
            });
        }
    }
    return out;
}

std::string ocr_markdown_for_row(const nlohmann::json& row)
{
    std::ostringstream md;
    md << "## " << media::path::path_to_utf8(media::path::path_from_utf8(row.value("path", std::string{})).filename()) << "\n\n";
    if (row.contains("layout")) {
        const auto& boxes = row["layout"].value("boxes", nlohmann::json::array());
        md << "**Layout:** " << (row["layout"].value("ok", false) ? "ok" : "failed")
           << " (" << boxes.size() << " regions)  \n\n";
        if (!boxes.empty()) {
            md << "| # | label | score | x0 | y0 | x1 | y1 |\n";
            md << "|---|-------|-------|-----|-----|-----|-----|\n";
            for (const auto& b : boxes) {
                md << "| " << b.value("readOrder", 0)
                   << " | " << md_cell(b.value("label", std::string{}))
                   << " | " << b.value("score", 0.0)
                   << " | " << static_cast<int>(b.value("x0", 0.0))
                   << " | " << static_cast<int>(b.value("y0", 0.0))
                   << " | " << static_cast<int>(b.value("x1", 0.0))
                   << " | " << static_cast<int>(b.value("y1", 0.0)) << " |\n";
            }
            md << "\n";
        }
    }
    md << "**VLM prompt:** `" << row.value("prompt", std::string{}) << "`  \n\n";
    const auto rows = row.value("tableRows", nlohmann::json::array());
    if (!rows.empty()) {
        md << "### Table rows\n\n";
        const std::size_t cols = rows.front().is_array() ? rows.front().size() : 0;
        md << "| ";
        for (std::size_t i = 0; i < cols; ++i) md << "Col " << (i + 1) << " | ";
        md << "\n| ";
        for (std::size_t i = 0; i < cols; ++i) md << "--- | ";
        md << "\n";
        for (const auto& tr : rows) {
            md << "| ";
            for (std::size_t i = 0; i < cols; ++i)
                md << md_cell(i < tr.size() && tr[i].is_string() ? tr[i].get<std::string>() : std::string{}) << " | ";
            md << "\n";
        }
        md << "\n";
        if (row.contains("receipt")) {
            md << "### Structured receipt\n\n```json\n"
               << row["receipt"].dump(2) << "\n```\n";
        }
    } else {
        md << "### OCR text\n\n```\n" << row.value("text", std::string{}) << "\n```\n";
    }
    return md.str();
}

BlockResult run_onnx_ocr(BlockIO& io,
                         const std::vector<std::string>& inputs,
                         const std::string& provider,
                         const std::string& model_preset)
{
#if !defined(FEATURE_ONNX) || !FEATURE_ONNX
    (void)io; (void)inputs; (void)provider; (void)model_preset;
    return BlockResult::Error;
#else
    std::string det_model = io.str("detModel");
    std::string rec_model = io.str("recModel");
    std::string dict_path = io.str("dict");
    apply_ocr_onnx_preset(model_preset, det_model, rec_model, dict_path);
    if (!file_exists(det_model)) {
        io.error("ocrText", "ocrText: ONNX detection model not found"
                             + (det_model.empty() ? std::string{} : (": " + det_model))
                             + "\n" + ocr_onnx_preset_not_found_message(model_preset), 1);
        return BlockResult::Error;
    }
    if (!file_exists(rec_model)) {
        io.error("ocrText", "ocrText: ONNX recognition model not found"
                             + (rec_model.empty() ? std::string{} : (": " + rec_model))
                             + "\n" + ocr_onnx_preset_not_found_message(model_preset), 1);
        return BlockResult::Error;
    }
    if (!file_exists(dict_path)) {
        io.error("ocrText", "ocrText: OCR character dictionary not found"
                             + (dict_path.empty() ? std::string{} : (": " + dict_path))
                             + "\n" + ocr_onnx_preset_not_found_message(model_preset), 1);
        return BlockResult::Error;
    }

    const std::vector<std::string> dict = media::onnx::ocr_load_char_dict(dict_path);
    if (dict.empty()) {
        io.error("ocrText", "ocrText: OCR character dictionary is empty or unreadable: " + dict_path, 1);
        return BlockResult::Error;
    }

    const int threads = static_cast<int>(io.i64("threads", 4));
    const auto ep = media::onnx::parse_provider(provider);
    media::onnx::OnnxSession det_session;
    if (!det_session.load(det_model, threads, ep)) {
        io.error("ocrText", "ocrText: failed to load ONNX detection model: " + det_session.error(), 1);
        return BlockResult::Error;
    }
    media::onnx::OnnxSession rec_session;
    if (!rec_session.load(rec_model, threads, ep)) {
        io.error("ocrText", "ocrText: failed to load ONNX recognition model: " + rec_session.error(), 1);
        return BlockResult::Error;
    }

    const float det_threshold = static_cast<float>(io.f64("threshold", 0.3));
    const float rec_threshold = static_cast<float>(io.f64("recThreshold", 0.5));
    const int max_size = static_cast<int>(io.i64("maxSize", 960));
    const bool json_output = io.flag("json", true);

    nlohmann::json rows = nlohmann::json::array();
    int ok_count = 0;
    int fail_count = 0;
    std::string first_text;

    for (const std::string& input : inputs) {
        if (io.cancelled())
            return BlockResult::Fatal;

        nlohmann::json row{{"path", input}, {"provider", "onnx"}};
        row["requestedProvider"] = provider;
        row["activeProvider"] = media::onnx::provider_name(det_session.active_provider());

        const auto result = media::onnx::onnx_ocr_full(
            det_session, rec_session, dict, input, det_threshold, max_size, rec_threshold);
        if (!result.ok) {
            row["ok"] = false;
            row["error"] = result.error;
            ++fail_count;
            rows.push_back(std::move(row));
            continue;
        }

        row["ok"] = true;
        row["text"] = result.page.text;
        row["page"] = page_json(result.page);
        row["boxCount"] = result.box_count;
        row["recCount"] = result.rec_count;
        row["timing"] = {
            {"detMs", result.timing.det_ms},
            {"recMs", result.timing.rec_ms},
            {"totalMs", result.timing.total_ms},
        };
        if (first_text.empty())
            first_text = result.page.text;
        ++ok_count;
        rows.push_back(std::move(row));
    }

    if (fail_count > 0) {
        io.error("ocrText", "ocrText: " + std::to_string(fail_count) + " image(s) failed", 1)
          .set("ok", static_cast<long long>(ok_count))
          .set("failed", static_cast<long long>(fail_count))
          .set("results", Value::from_json(rows));
        return BlockResult::Error;
    }

    nlohmann::json result = json_output
        ? (rows.size() == 1 ? rows.front() : rows)
        : nlohmann::json(first_text);
    io.ok("ocrText", "OCR complete for " + std::to_string(ok_count) + " image(s)")
      .set("provider", std::string("onnx"))
      .set("model", model_preset.empty() ? std::string("custom") : model_preset)
      .set("activeProvider", std::string(media::onnx::provider_name(det_session.active_provider())))
      .set("count", static_cast<long long>(ok_count))
      .set("results", Value::from_json(rows))
      .result(Value::from_json(result), json_output ? ParamKind::json_value : ParamKind::string_);
    return BlockResult::Ok;
#endif
}

BlockResult run_vlm_ocr(BlockIO& io,
                        const std::vector<std::string>& inputs,
                        const std::string& model,
                        const std::string& provider_label)
{
    auto log_level_off = [&io]() {
        static constexpr const char kLogLevelPrefix[] = "--log-level=";
        const auto& args = io.runtime().options.process_args;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string& arg = args[i];
            if (arg == "--log-level") {
                if (i + 1 < args.size() && lower_ascii(args[i + 1]) == "off")
                    return true;
                ++i;
            } else if (arg.rfind(kLogLevelPrefix, 0) == 0) {
                if (lower_ascii(arg.substr(sizeof(kLogLevelPrefix) - 1)) == "off")
                    return true;
            }
        }
        return false;
    };

    if (!media::llm::vlm_client::is_vlm_available("ocr") && model.empty()) {
        io.error("ocrText", "ocrText: " + ocr_vlm_not_found_message(model), 1);
        return BlockResult::Error;
    }

    media::llm::vlm_client::VlmRequest req;
    req.image_paths = inputs;
    req.prompt = io.str("prompt", "OCR: Extract all text verbatim, preserving line breaks and spatial layout.");
    req.model_id = model;
    req.slot = "ocr";
    req.n_predict = static_cast<int>(io.i64("maxTokens", 2048));
    req.n_threads = static_cast<int>(io.i64("threads", 4));
    req.n_ctx = static_cast<int>(io.i64("ctx", 8192));
    req.n_gpu_layers = static_cast<int>(io.i64("gpuLayers", -1));
    req.cancel_check = [&io] { return io.cancelled(); };
    req.stream_tokens = !log_level_off();

    const auto res = media::llm::vlm_client::execute(req);
    if (io.cancelled())
        return BlockResult::Fatal;
    if (!res.ok) {
        io.error("ocrText", "ocrText: " + res.error, 1);
        return BlockResult::Error;
    }

    const bool json_output = io.flag("json", true);
    nlohmann::json payload{
        {"ok", true},
        {"text", res.text},
        {"provider", provider_label.empty() ? "llama" : provider_label},
        {"model", res.model_id.empty() ? model : res.model_id},
        {"modelPath", res.model_path},
        {"mmprojPath", res.mmproj_path},
        {"elapsedMs", res.elapsed_ms},
        {"nGpuLayers", res.n_gpu_layers},
        {"paths", inputs},
    };

    io.ok("ocrText", "OCR complete")
      .set("provider", payload["provider"].get<std::string>())
      .set("model", payload.value("model", std::string{}))
      .set("elapsedMs", ParamKind::float_, Value{static_cast<double>(res.elapsed_ms)})
      .set("nGpuLayers", ParamKind::integer_, Value{static_cast<long long>(res.n_gpu_layers)})
      .payload(payload)
      .result(json_output ? Value::from_json(payload) : Value{res.text},
              json_output ? ParamKind::json_value : ParamKind::string_);
    return BlockResult::Ok;
}

BlockResult run_layout_vlm_ocr(BlockIO& io,
                              const std::vector<std::string>& inputs,
                              const std::string& model)
{
#if !defined(FEATURE_ONNX) || !FEATURE_ONNX
    (void)io; (void)inputs; (void)model;
    io.error("ocrText", "ocrText: layout OCR requires FEATURE_ONNX", 1);
    return BlockResult::Error;
#else
    const auto layout_probe = media::local_models::probe_layout_onnx_model_path(io.str("layoutModel"));
    if (!file_exists(layout_probe.path)) {
        io.error("ocrText", "ocrText: " + media::local_models::layout_onnx_model_not_found_message(layout_probe), 1);
        return BlockResult::Error;
    }
    const std::string& layout_model = layout_probe.path;
    const std::string vlm_model = model.empty() ? preferred_ocr_vlm_model() : model;
    if (!media::llm::vlm_client::is_vlm_available("ocr") && vlm_model.empty()) {
        io.error("ocrText", "ocrText: " + ocr_vlm_not_found_message(model), 1);
        return BlockResult::Error;
    }

    const int threads = static_cast<int>(io.i64("threads", 4));
    const auto ep = media::onnx::parse_provider(io.str("backend", "auto"));
    media::onnx::OnnxSession layout_session;
    if (!layout_session.load(layout_model, threads, ep)) {
        io.error("ocrText", "ocrText: failed to load PP-DocLayoutV3 model: " + layout_session.error(), 1);
        return BlockResult::Error;
    }

    const std::string provider_mode = lower_ascii(io.str("provider"));
    const std::string mode = (provider_mode == "table" || provider_mode == "text" || provider_mode == "layout")
        ? provider_mode
        : lower_ascii(io.str("mode", "layout"));
    const bool include_markdown = io.flag("markdown", true);
    const bool json_output = io.flag("json", true);
    const float conf = static_cast<float>(io.f64("layoutConf", 0.5));
    nlohmann::json docs = nlohmann::json::array();
    std::string combined_markdown;
    int ok_count = 0;

    for (const auto& input : inputs) {
        if (io.cancelled())
            return BlockResult::Fatal;

        const auto layout = media::onnx::onnx_doc_layout(layout_session, input, conf);
        const bool has_table = layout.ok && layout_has_table(layout);
        const bool force_table = mode == "table";
        const bool force_text = mode == "text" || mode == "ocr";
        const std::string prompt = force_table || (!force_text && has_table)
            ? "Table Recognition:"
            : "OCR:";

        media::llm::vlm_client::VlmRequest req;
        req.image_paths = { input };
        req.prompt = prompt;
        req.model_id = vlm_model;
        req.slot = "ocr";
        req.n_predict = static_cast<int>(io.i64("maxTokens", 2048));
        req.n_threads = threads;
        req.n_ctx = static_cast<int>(io.i64("ctx", 8192));
        req.n_gpu_layers = static_cast<int>(io.i64("gpuLayers", -1));
        req.cancel_check = [&io] { return io.cancelled(); };

        const auto vlm = media::llm::vlm_client::execute(req);
        if (io.cancelled())
            return BlockResult::Fatal;

        nlohmann::json layout_j{
            {"ok", layout.ok},
            {"model", media::path::path_to_utf8(media::path::path_from_utf8(layout_model).filename())},
            {"activeProvider", media::onnx::provider_name(layout_session.active_provider())},
            {"boxes", nlohmann::json::array()},
            {"timing", {
                {"preprocessMs", layout.timing.preprocess_ms},
                {"inferenceMs", layout.timing.inference_ms},
                {"postprocMs", layout.timing.postproc_ms},
                {"totalMs", layout.timing.total_ms},
            }},
        };
        if (!layout.ok)
            layout_j["error"] = layout.error;
        for (const auto& box : layout.boxes)
            layout_j["boxes"].push_back(layout_box_json(box));

        nlohmann::json doc{
            {"ok", vlm.ok},
            {"path", input},
            {"provider", "layout-vlm"},
            {"mode", mode},
            {"prompt", prompt},
            {"layout", std::move(layout_j)},
            {"model", vlm.model_id.empty() ? vlm_model : vlm.model_id},
            {"modelPath", vlm.model_path},
            {"mmprojPath", vlm.mmproj_path},
            {"elapsedMs", vlm.elapsed_ms},
            {"nGpuLayers", vlm.n_gpu_layers},
            {"text", vlm.text},
        };

        if (!vlm.ok) {
            doc["error"] = vlm.error;
        } else {
            ++ok_count;
            if (prompt == "Table Recognition:") {
                const auto table_rows = parse_table_tokens(vlm.text);
                doc["tableRows"] = table_rows;
                if (!table_rows.empty())
                    doc["receipt"] = receipt_fields_from_rows(table_rows);
            } else {
                doc["tableRows"] = nlohmann::json::array();
            }
        }
        const std::string doc_markdown = ocr_markdown_for_row(doc);
        if (include_markdown)
            doc["markdown"] = doc_markdown;
        if (!combined_markdown.empty())
            combined_markdown += "\n";
        combined_markdown += doc_markdown;
        docs.push_back(std::move(doc));
    }

    if (ok_count == 0) {
        io.error("ocrText", "ocrText: no images processed successfully", 1)
          .set("documents", Value::from_json(docs));
        return BlockResult::Error;
    }

    const Value result = json_output
        ? Value::from_json(docs)
        : Value{combined_markdown};
    io.ok("ocrText", "OCR layout pipeline complete for " + std::to_string(ok_count) + " image(s)")
      .set("provider", std::string("layout-vlm"))
      .set("model", docs.front().value("model", std::string{}))
      .set("count", static_cast<long long>(ok_count))
      .set("documents", ParamKind::json_value, Value::from_json(docs))
      .result(result, json_output ? ParamKind::json_value : ParamKind::string_);
    return BlockResult::Ok;
#endif
}

BlockResult ocr_text_block(BlockIO& io)
{
    const std::string event_kind = io.raw().is_object() ? io.raw().value("kind", std::string("ocrText")) : std::string("ocrText");
    io.event(event_kind, "running", "ocr starting");

    std::vector<std::string> inputs = strings_from_param(io, "input");
    if (inputs.empty()) {
        io.error("ocrText", "ocrText: provide input image path(s)", 1);
        return BlockResult::Error;
    }

    const std::string block_provider = lower_ascii(io.str("provider"));
    const std::string block_model = io.str("model");
    std::string provider = block_provider;
    std::string model = block_model;
    if (provider.empty() || model.empty())
        apply_ocr_settings_defaults(provider, model);
    provider = lower_ascii(provider);
    if (provider.empty())
        provider = "auto";

    if (provider == "auto" || provider == "layout" || provider == "table" || provider == "text") {
        return run_layout_vlm_ocr(io, inputs, model);
    }

    if (provider == "paddle") {
        std::string det_model = io.str("detModel");
        std::string rec_model = io.str("recModel");
        std::string dict_path = io.str("dict");
        const std::string onnx_model = block_model.empty() ? std::string("auto") : block_model;
        if (apply_ocr_onnx_preset(onnx_model, det_model, rec_model, dict_path))
            return run_onnx_ocr(io, inputs, io.str("backend", "auto"), onnx_model);
        io.error("ocrText", "ocrText: " + ocr_onnx_preset_not_found_message(onnx_model), 1);
        return BlockResult::Error;
    }

    if (provider == "llama" || provider == "vlm") {
        return run_vlm_ocr(io, inputs, model, provider);
    }

    io.error("ocrText", "ocrText: unsupported xBlox OCR provider '" + provider +
                        "' (supported: auto, layout, table, text, vlm, paddle)", 1);
    return BlockResult::Error;
}

} // namespace

void register_ocr_blocks(BlockRegistry& registry)
{
    std::vector<ParamDef> ocr_params = {
        pd_image("input").in().grp("source_model").prev().globs().req()
            .desc("Input image path or glob. Default: PREVIOUS chain value."),
        pd_enum("provider", {"auto", "layout", "table", "text", "vlm", "paddle"}).grp("source_model").dflt("auto")
            .desc("OCR runtime. Default: auto (PP-DocLayoutV3 + PaddleOCR-VL). "
                  "layout/table/text force the VLM prompt; paddle uses raw ONNX PaddleOCR. "
                  "Empty falls back to App Settings ocr_provider, then image_recognition_provider."),
        pd_enum("mode", {"layout", "table", "text"}).grp("source_model").dflt("layout")
            .desc("Layout-table routing when provider=auto. Default: layout "
                  "(detect tables first, then choose OCR: vs Table Recognition:)."),
        pd_model().grp("source_model").lbl("Model").dflt("")
            .ui({{"control", "select"}, {"options_path", "ocr.models.vlm"}, {"empty_label", "(auto OCR VLM)"}})
            .desc("Local OCR VLM model id or GGUF path. Default: empty → first OCR-slot VLM "
                  "(prefers PaddleOCR-VL), else App Settings ocr_model / image_recognition_model."),
        pd_prompt("prompt").in().adv().grp("vlm_prompt")
            .dflt("OCR: Extract all text verbatim, preserving line breaks and spatial layout.")
            .desc("VLM prompt override for provider=vlm only. Default: OCR: Extract all text verbatim… "
                  "Layout/table/text providers choose OCR: or Table Recognition: automatically."),
        pd_bool("json").grp("output").dflt(true)
            .desc("Structured JSON document array vs plain markdown. Default: true."),
        pd_bool("markdown").grp("output").dflt(true)
            .desc("Include markdown on each JSON document. Default: true."),
        pd_enum("backend", {"auto", "cpu", "gpu", "cuda", "coreml", "metal"}).grp("runtime").dflt("auto")
            .desc("ONNX Runtime EP for PP-DocLayoutV3 / PaddleOCR ONNX. Default: auto."),
        pd_int("threads").grp("runtime").dflt(4).range(1, 64)
            .desc("CPU threads for ONNX and VLM inference. Default: 4."),
        pd_file("layoutModel").adv().grp("layout_model").dflt("")
            .desc("Override PP-DocLayoutV3.onnx path. Default: empty → auto-discover PP-DocLayoutV3.onnx "
                  "under ${MODELS_DIR} then install models (exe/../models)."),
        pd_float("layoutConf").adv().grp("layout_model").dflt(0.5).range(0, 1)
            .desc("Minimum PP-DocLayoutV3 region confidence. Default: 0.5."),
        pd_file("detModel").adv().grp("onnx_model_overrides").dflt("")
            .desc("Override PaddleOCR det ONNX path. Default: empty → en_PP-OCRv3_det_infer.onnx "
                  "from paddleocr-en preset under model roots."),
        pd_file("recModel").adv().grp("onnx_model_overrides").dflt("")
            .desc("Override PaddleOCR rec ONNX path. Default: empty → en_PP-OCRv4_rec_infer.onnx "
                  "(or v3) from paddleocr-en preset under model roots."),
        pd_file("dict").adv().grp("onnx_model_overrides").dflt("")
            .desc("Override OCR character dictionary. Default: empty → en_dict.txt from paddleocr-en preset."),
        pd_float("threshold").adv().grp("onnx_options").dflt(0.3).range(0, 1)
            .desc("PaddleOCR detection probability threshold. Default: 0.3."),
        pd_float("recThreshold").adv().grp("onnx_options").dflt(0.5).range(0, 1)
            .desc("PaddleOCR recognition confidence threshold. Default: 0.5."),
        pd_int("maxSize").adv().grp("onnx_options").dflt(960).range(32, 4096)
            .desc("PaddleOCR detector max image side after resize (px). Default: 960."),
        pd_int("maxTokens").adv().grp("vlm_runtime").dflt(2048).range(1, 32768)
            .desc("VLM max generated tokens. Default: 2048."),
        pd_int("ctx").adv().grp("vlm_runtime").dflt(8192).range(512, 131072)
            .desc("VLM context length. Default: 8192."),
        pd_int("gpuLayers").adv().grp("vlm_runtime").dflt(-1).range(-1, 999)
            .desc("VLM GPU layer offload (-1 = all, 0 = CPU). Default: -1."),
        pd_json("result").out().desc("OCR document array when json=true; markdown string when json=false."),
        pd_store_as().dflt("ocr")
            .desc("Variable name for PREVIOUS / downstream blocks. Default: ocr."),
    };
    const ExecFlags ocr_exec{true, true, true};

    registry["ocrText"] = bdb(
        "ocrText", ocr_text_block,
        "OCR Text", "OCR",
        "Ready-to-go document OCR: PP-DocLayoutV3 detects layout, PaddleOCR-VL extracts text/tables, and child items can iterate documents.",
        {{"kind","ocrText"}, {"input",""}, {"provider","auto"}, {"mode","layout"}, {"model",""},
         {"prompt","OCR: Extract all text verbatim, preserving line breaks and spatial layout."},
         {"json",true}, {"markdown",true}, {"backend","auto"}, {"layoutModel",""}, {"layoutConf",0.5},
         {"detModel",""}, {"recModel",""}, {"dict",""}, {"threshold",0.3}, {"recThreshold",0.5},
         {"maxSize",960}, {"threads",4}, {"maxTokens",2048}, {"ctx",8192}, {"gpuLayers",-1},
         {"storeAs","ocr"}, {"items", nlohmann::json::array()}})
        .params(ocr_params)
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::json_value, ParamKind::string_},
            "OCR result")
        .output("documents", ParamKind::json_value, "Documents", "documents", true)
        .output("elapsedMs", ParamKind::float_, "Elapsed time", "elapsedMs", true)
        .output("nGpuLayers", ParamKind::integer_, "GPU layers", "nGpuLayers", true)
        .children({{"items", "For each document", "body", {"*"}, 0, -1}})
        .exec(ocr_exec)
        .iterable();

    registry["ocr"] = bdb(
        "ocr", ocr_text_block,
        "", "OCR",
        "Alias for ocrText.",
        {{"kind","ocr"}, {"input",""}, {"provider","auto"}, {"mode","layout"}, {"model",""},
         {"prompt","OCR: Extract all text verbatim, preserving line breaks and spatial layout."},
         {"json",true}, {"markdown",true}, {"backend","auto"}, {"layoutModel",""}, {"layoutConf",0.5},
         {"detModel",""}, {"recModel",""}, {"dict",""}, {"threshold",0.3}, {"recThreshold",0.5},
         {"maxSize",960}, {"threads",4}, {"maxTokens",2048}, {"ctx",8192}, {"gpuLayers",-1},
         {"storeAs","ocr"}, {"items", nlohmann::json::array()}})
        .params(ocr_params)
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::json_value, ParamKind::string_},
            "OCR result")
        .output("documents", ParamKind::json_value, "Documents", "documents", true)
        .output("elapsedMs", ParamKind::float_, "Elapsed time", "elapsedMs", true)
        .output("nGpuLayers", ParamKind::integer_, "GPU layers", "nGpuLayers", true)
        .children({{"items", "For each document", "body", {"*"}, 0, -1}})
        .exec(ocr_exec)
        .iterable()
        .hidden();
}

} // namespace media::xblox::blocks

#else

namespace media::xblox::blocks {

void register_ocr_blocks(BlockRegistry&) {}

} // namespace media::xblox::blocks

#endif
