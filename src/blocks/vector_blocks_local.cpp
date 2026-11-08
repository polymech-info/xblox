// vector_blocks_local.cpp  -  Local (offline) GGUF-backed vector store XBlox blocks.
//
// Blocks: vectorLocalOpen · vectorLocalAddText · vectorLocalAddFile ·
//         vectorLocalAddDir · vectorLocalSearch · vectorLocalDelete · vectorLocalStats
//
// All embedding is done locally via llama.cpp (EmbeddingModel / GGUF).
// No network calls, no LiteLLM, no server.
//
// Store registry:
//   A process-scope map<string, shared_ptr<VectorStore>> caches open stores by
//   their resolved absolute directory path. Within a single xblox run, all blocks
//   that reference the same storePath share one loaded model instance.
//
// Feature guard: FEATURE_VECTOR_LOCAL_XBLOX=1 (requires FEATURE_LLAMA).

#include "blocks/vector_blocks_local.hpp"
#include "blocks/block_registry.hpp"
#include "blocks/block_params.hpp"
#include "fs/file_extensions.hpp"
#include "fs/path_sanitizer.hpp"

#if defined(FEATURE_VECTOR_LOCAL_XBLOX) && FEATURE_VECTOR_LOCAL_XBLOX
#if !defined(FEATURE_LLAMA) || !FEATURE_LLAMA
#  error "FEATURE_VECTOR_LOCAL_XBLOX requires FEATURE_LLAMA"
#endif

#include "lib/llm/local_vector_store/VectorStore.hpp"
#include "lib/llm/chunking.hpp"
#include "lib/llm/chunking_md.hpp"
#include "fs/fs_guard.hpp"
#include "fs/glob_filter.hpp"
#if defined(FEATURE_OFFICE_EXTRACTOR) && FEATURE_OFFICE_EXTRACTOR
#  include "lib/llm/office_source_adapter.hpp"
#endif

#include <nlohmann/json.hpp>

#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "string/string_utils.hpp"

namespace media::xblox::blocks {
namespace {

namespace fs = std::filesystem;
using J      = nlohmann::json;

std::wstring lower_ext(const fs::path& path)
{
    std::wstring ext = path.extension().wstring();
    for (auto& c : ext)
        c = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
    return ext;
}

// ── Process-scope store registry ──────────────────────────────────────────────

struct StoreEntry {
    std::shared_ptr<media::llm::VectorStore> store;
    std::string model_path;       // what was requested (hint used at open time)
    int         gpu_layers = 0;   // GPU offload used at open time
};

static std::mutex                                   g_registry_mu;
static std::unordered_map<std::string, StoreEntry>  g_registry;

// Open or return cached store for `dir_abs`.
// A cached entry is reused only when the model hint AND the GPU offload match;
// otherwise it is evicted and reopened (changing either requires a model reload).
std::shared_ptr<media::llm::VectorStore>
get_or_open_store(const std::string& dir_abs, const std::string& model_hint,
                  int n_threads, int n_gpu_layers, std::string& err_out)
{
    std::lock_guard<std::mutex> lk(g_registry_mu);

    auto it = g_registry.find(dir_abs);
    if (it != g_registry.end()) {
        if (it->second.model_path == model_hint
            && it->second.gpu_layers == n_gpu_layers
            && it->second.store->loaded())
            return it->second.store;
        g_registry.erase(it);   // model/GPU changed  -  evict and reload
    }

    auto vs = std::make_shared<media::llm::VectorStore>();
    if (!vs->open(media::path::path_from_utf8(dir_abs), model_hint, n_threads, n_gpu_layers)) {
        err_out = vs->error();
        return nullptr;
    }

    g_registry.emplace(dir_abs, StoreEntry{vs, model_hint, n_gpu_layers});
    return vs;
}

// ── Param helpers ──────────────────────────────────────────────────────────────

// Common params shared by all local vector blocks
std::vector<ParamDef> vl_common_params()
{
    return {
        pd_str("storePath").in().req().deep().dflt("").desc(
            "Directory for the local vector store (created if absent). Supports ${var}."),
        pd_model().desc(
            "Path to GGUF embedding model. Empty = auto-discover (PM_EMBED_MODEL, ${MODELS_DIR}, exe/../models)."),
        pd_int("threads").grp("advanced").dflt(4).range(1, 32).desc(
            "CPU threads for llama.cpp embedding inference."),
        pd_int("gpuLayers").grp("advanced").dflt(0).range(-1, 999).desc(
            "GPU offload for embedding: 0 = CPU only, -1 = all layers on GPU, "
            ">0 = first N layers. Speeds up large ingests on CUDA/Metal/Vulkan builds."),
    };
}

// Read the GPU-offload count from the block (clamped to the param range).
inline int vl_gpu_layers(BlockIO& io) {
    return static_cast<int>(io.i64("gpuLayers", 0));
}

ParamDef vl_chunk_size_param() {
    return pd_int("chunkSize").grp("chunking").dflt(500).range(50, 4000)
        .desc("Max characters per chunk when splitting long text (approx 100-800 tokens).");
}
ParamDef vl_chunk_overlap_param() {
    return pd_int("chunkOverlap").grp("chunking").dflt(100).range(0, 500)
        .desc("Overlap characters between adjacent chunks.");
}
ParamDef vl_timeout_param() {
    return pd_int("timeoutMs").grp("advanced").dflt(120000).range(1000, 600000)
        .desc("Per-block timeout in ms (embedding large files can take seconds).");
}
ParamDef vl_store_as(const char* dflt = "result") {
    return pd_store_as().dflt(dflt);
}

// Resolve store path (deep-resolve + absolute)
std::string resolve_store_path(const std::string& raw)
{
    if (raw.empty()) return {};
    std::error_code ec;
    const fs::path abs = fs::absolute(media::path::path_from_utf8(raw), ec);
    return ec ? raw : media::path::path_to_utf8(abs);
}

// Build a standard "stats" JSON object from a loaded store
J store_stats_json(const media::llm::VectorStore& vs)
{
    J j;
    j["docCount"]  = vs.doc_count();
    j["dims"]      = vs.dims();
    j["modelPath"] = vs.model_path();
    j["gpu"]       = vs.gpu();
    j["gpuLayers"] = vs.gpu_layers();
    return j;
}

// Resolve storePath + open (or reuse) the store with the block's model/threads/GPU
// settings. On failure, emits the block error and returns nullptr; on success,
// writes the resolved absolute path into `dir_abs_out`. Centralizes the
// resolve->open boilerplate every vector block repeats.
std::shared_ptr<media::llm::VectorStore>
vl_open(BlockIO& io, const char* K, std::string& dir_abs_out)
{
    const std::string dir_abs = resolve_store_path(io.str("storePath"));
    if (dir_abs.empty()) { io.error(K, "storePath is required", 2); return nullptr; }

    std::string err;
    auto vs = get_or_open_store(dir_abs, io.str("model"),
                                static_cast<int>(io.i64("threads", 4)),
                                vl_gpu_layers(io), err);
    if (!vs) {
        io.error(K, "failed to open store at " + dir_abs + ": " + err, 1)
          .set("storePath", dir_abs);
        return nullptr;
    }
    dir_abs_out = dir_abs;
    return vs;
}

// ── vectorLocalOpen ────────────────────────────────────────────────────────────
//
// Open (or create) a local vector store and load the embedding model.
// PREVIOUS = {docCount, dims, modelPath}.

BlockResult vector_local_open_block(BlockIO& io)
{
    constexpr const char* K = "vectorLocalOpen";

    std::string dir_abs;
    const auto vs = vl_open(io, K, dir_abs);
    if (!vs) return BlockResult::Error;

    const J stats = store_stats_json(*vs);
    io.ok(K, std::to_string(vs->doc_count()) + " docs · " + std::to_string(vs->dims())
             + " dims" + (vs->gpu() ? " · GPU" : ""))
      .set("storePath",  dir_abs)
      .set("docCount",   ParamKind::integer_, Value{static_cast<long long>(vs->doc_count())})
      .set("dims",       ParamKind::integer_, Value{static_cast<long long>(vs->dims())})
      .set("modelPath",  vs->model_path())
      .set("gpu",        ParamKind::boolean_, Value{vs->gpu()})
      .result(Value::from_json(stats), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── vectorLocalAddText ────────────────────────────────────────────────────────
//
// Embed a text string and insert it into the local vector store.
// Long texts are automatically chunked.
// PREVIOUS = {ids:[...], count:N, storePath}.

BlockResult vector_local_add_text_block(BlockIO& io)
{
    constexpr const char* K = "vectorLocalAddText";

    const std::string text = io.str("text");
    if (text.empty()) { io.error(K, "text param is required", 2); return BlockResult::Error; }

    std::string dir_abs;
    const auto vs = vl_open(io, K, dir_abs);
    if (!vs) return BlockResult::Error;

    const std::string source = io.str("source", "text");
    const std::string path   = io.str("path");
    const std::string title  = io.str("title");
    const int chunk_size    = static_cast<int>(io.i64("chunkSize",   500));
    const int chunk_overlap = static_cast<int>(io.i64("chunkOverlap", 100));

    // Parse tags array param
    std::vector<std::string> tags;
    {
        const std::string tags_str = io.str("tags");
        if (!tags_str.empty()) {
            try {
                const J tj = J::parse(tags_str);
                if (tj.is_array())
                    for (const auto& t : tj)
                        if (t.is_string()) tags.push_back(t.get<std::string>());
            } catch (...) { tags.push_back(tags_str); }
        }
    }

    const auto ids = vs->add_chunked(text, source, path, title, tags, chunk_size, chunk_overlap);
    if (ids.empty() && !vs->error().empty()) {
        io.error(K, vs->error(), 1).set("storePath", dir_abs);
        return BlockResult::Error;
    }

    if (!vs->save()) {
        io.error(K, "save failed: " + vs->error(), 1).set("storePath", dir_abs);
        return BlockResult::Error;
    }

    J result;
    result["ids"]       = ids;
    result["count"]     = ids.size();
    result["storePath"] = dir_abs;
    result["docCount"]  = vs->doc_count();

    io.ok(K, std::to_string(ids.size()) + " chunk(s) added -> " + dir_abs)
      .set("storePath", dir_abs)
      .set("count",     ParamKind::integer_, Value{static_cast<long long>(ids.size())})
      .set("docCount",  ParamKind::integer_, Value{static_cast<long long>(vs->doc_count())})
      .result(Value::from_json(result), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── vectorLocalAddFile ────────────────────────────────────────────────────────
//
// Read a local text file, chunk it, embed each chunk, insert into the store.
// Uses the same FS guard as llm embed (blocks binary/image files, > 1 MiB).
// PREVIOUS = {ids:[...], count:N, file, storePath}.

constexpr std::uintmax_t k_vl_max_file_bytes = 1024ULL * 1024;

BlockResult vector_local_add_file_block(BlockIO& io)
{
    constexpr const char* K = "vectorLocalAddFile";

    const std::string raw_store = io.str("storePath");
    if (resolve_store_path(raw_store).empty()) { io.error(K, "storePath is required", 2); return BlockResult::Error; }

    const std::string raw_file = io.str("file");
    if (raw_file.empty()) { io.error(K, "file param is required", 2); return BlockResult::Error; }

    // Resolve + guard the file path
    std::error_code ec;
    fs::path abs_file = fs::absolute(media::path::path_from_utf8(raw_file), ec);
    if (ec) { io.error(K, "cannot resolve file path: " + ec.message(), 2); return BlockResult::Error; }
    {
        fs::path can = fs::weakly_canonical(abs_file, ec);
        if (!ec && !can.empty()) abs_file = std::move(can);
    }

    if (!fs::is_regular_file(abs_file, ec)) {
        io.error(K, "not a regular file: " + media::path::path_to_utf8(abs_file), 2);
        return BlockResult::Error;
    }

    const std::string deny = media::llm::llm_fs_guard_deny_reason(abs_file);
    if (!deny.empty()) { io.error(K, deny, 2); return BlockResult::Error; }

    const std::wstring file_ext_w = lower_ext(abs_file);

    // Office formats are handled via OfficeSourceAdapter (text extracted at runtime).
    const bool is_office = pmui::is_office_document_ext(file_ext_w);
    const std::string file_ext = media::path::path_to_utf8(abs_file.extension());
    if (!is_office && media::llm::glob_file_extension_blocked_like_file_read(file_ext)) {
        io.error(K, "refusing binary or image extension: " + file_ext, 2);
        return BlockResult::Error;
    }

    // Office files: text is extracted by OfficeSourceAdapter (no direct read).
    // Plain-text / markdown files: read normally within the 1 MiB guard.
    std::string text; // plain-text content for the non-office path

    if (!is_office) {
        const std::uintmax_t sz = fs::file_size(abs_file, ec);
        if (ec) { io.error(K, "cannot read file size: " + ec.message(), 2); return BlockResult::Error; }
        if (sz > k_vl_max_file_bytes) {
            io.error(K, "file exceeds 1 MiB embed limit: " + media::path::path_to_utf8(abs_file), 2);
            return BlockResult::Error;
        }
        {
            std::ifstream in(abs_file, std::ios::binary);
            if (!in) { io.error(K, "cannot open file: " + media::path::path_to_utf8(abs_file), 2); return BlockResult::Error; }
            text.resize(static_cast<std::size_t>(sz));
            if (sz > 0) {
                in.read(text.data(), static_cast<std::streamsize>(sz));
                if (static_cast<std::uintmax_t>(in.gcount()) != sz) {
                    io.error(K, "short read: " + media::path::path_to_utf8(abs_file), 2);
                    return BlockResult::Error;
                }
            }
            if (text.find('\0') != std::string::npos) {
                io.error(K, "refusing NUL-containing binary content", 2);
                return BlockResult::Error;
            }
        }
    }

    std::string dir_abs;
    const auto vs = vl_open(io, K, dir_abs);
    if (!vs) return BlockResult::Error;

    const std::string source    = io.str("source", "file");
    const std::string file_path = media::path::path_to_utf8(abs_file);
    const int chunk_size        = static_cast<int>(io.i64("chunkSize",    500));
    const int chunk_overlap     = static_cast<int>(io.i64("chunkOverlap", 100));

    // Skip already-indexed files (deduplication across runs)
    if (vs->has_path(file_path)) {
        const J result = {{"ids",J::array()},{"count",0},
                          {"file",file_path},{"storePath",dir_abs},
                          {"docCount",vs->doc_count()},{"skipped",true}};
        io.ok(K, "skipped (already indexed): " + media::path::path_to_utf8(abs_file.filename()))
          .set("storePath", dir_abs)
          .set("file",      file_path)
          .set("count",     ParamKind::integer_, Value{0LL})
          .set("docCount",  ParamKind::integer_, Value{static_cast<long long>(vs->doc_count())})
          .result(Value::from_json(result), ParamKind::json_value);
        return BlockResult::Ok;
    }

    // Extra tags from the block param (may be augmented by frontmatter below)
    std::vector<std::string> extra_tags;
    {
        const std::string ts = io.str("tags");
        if (!ts.empty()) {
            try {
                const J tj = J::parse(ts);
                if (tj.is_array())
                    for (const auto& t : tj)
                        if (t.is_string()) extra_tags.push_back(t.get<std::string>());
            } catch (...) { extra_tags.push_back(ts); }
        }
    }

    // Choose chunker based on file extension
    media::llm::ChunkOptions copts;
    copts.max_chars     = chunk_size;
    copts.overlap_chars = chunk_overlap;
    copts.min_chars     = 30;

    const bool is_md = pmui::is_markdown_ext(file_ext_w);

    std::vector<media::llm::Chunk> chunks;

    if (is_office) {
#if defined(FEATURE_OFFICE_EXTRACTOR) && FEATURE_OFFICE_EXTRACTOR
        media::llm::OfficeSourceAdapter adapter;
        const auto units = adapter.extract(abs_file);
        if (units.empty()) {
            if (!adapter.error().empty())
                io.error(K, "office extraction failed: " + adapter.error(), 1);
            // Treat as empty file  -  0 chunks, no error propagated if adapter was silent.
            // The block returns ok with count=0 so the orchestrator can decide.
        } else {
            media::llm::PlainTextChunker chunker;
            for (const auto& unit : units) {
                auto uchunks = chunker.chunk(unit.uri, unit.text, copts);
                // Override title from the adapter (sheet name / slide title / doc title)
                for (auto& ch : uchunks) {
                    if (ch.title.empty() && !unit.title.empty())
                        ch.title = unit.title;
                }
                chunks.insert(chunks.end(),
                              std::make_move_iterator(uchunks.begin()),
                              std::make_move_iterator(uchunks.end()));
            }
        }
#else
        io.error(K, "office extraction not compiled in (build with FEATURE_OFFICE_EXTRACTOR=1)", 2);
        return BlockResult::Error;
#endif
    } else if (is_md) {
        media::llm::MarkdownChunker chunker;
        chunks = chunker.chunk(file_path, text, copts);
    } else {
        media::llm::PlainTextChunker chunker;
        chunks = chunker.chunk(file_path, text, copts);
    }

    // Infer title: use frontmatter title (first chunk) or filename
    std::string title = io.str("title");
    if (title.empty() && !chunks.empty() && !chunks[0].title.empty())
        title = chunks[0].title;
    if (title.empty())
        title = media::path::path_to_utf8(abs_file.filename());

    // Embed + insert each chunk
    std::vector<uint64_t> ids;
    ids.reserve(chunks.size());
    for (const auto& ch : chunks) {
        media::llm::VStoreDoc doc;
        doc.source = source;
        doc.path   = file_path;
        doc.title  = ch.title.empty() ? title : ch.title;
        doc.chunk  = ch.content;    // display content
        doc.tags   = ch.tags;
        for (const auto& t : extra_tags) doc.tags.push_back(t);

        // embedding_text carries title + tags + summary + content_head
        const uint64_t id = vs->add(std::move(doc), ch.embedding_text);
        if (id == 0 && !vs->error().empty()) {
            io.error(K, vs->error(), 1).set("file", file_path).set("storePath", dir_abs);
            return BlockResult::Error;
        }
        if (id != 0) ids.push_back(id);
    }

    // Fallback: if chunker produced nothing (empty file), no-op is fine
    if (!ids.empty()) {
        if (!vs->save()) {
            io.error(K, "save failed: " + vs->error(), 1);
            return BlockResult::Error;
        }
    }

    J result;
    result["ids"]       = ids;
    result["count"]     = ids.size();
    result["file"]      = file_path;
    result["storePath"] = dir_abs;
    result["docCount"]  = vs->doc_count();

    io.ok(K, std::to_string(ids.size()) + " chunk(s) from " + media::path::path_to_utf8(abs_file.filename()))
      .set("storePath", dir_abs)
      .set("file",      file_path)
      .set("count",     ParamKind::integer_, Value{static_cast<long long>(ids.size())})
      .set("docCount",  ParamKind::integer_, Value{static_cast<long long>(vs->doc_count())})
      .result(Value::from_json(result), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── vectorLocalAddDir ─────────────────────────────────────────────────────────
//
// Recursively index all supported files in a directory.
// Skips already-indexed paths (via has_path deduplication).
// PREVIOUS = {files:[{file,count,skipped}], totalChunks:N, storePath, docCount}.

// Supported extensions for directory indexing
static bool vl_is_supported_ext(const std::string& ext)
{
    std::wstring ext_w(ext.begin(), ext.end());
    return pmui::is_vector_ingest_text_ext(ext_w)
#if defined(FEATURE_OFFICE_EXTRACTOR) && FEATURE_OFFICE_EXTRACTOR
        || pmui::is_office_document_ext(ext_w)
#endif
        ;
}

BlockResult vector_local_add_dir_block(BlockIO& io)
{
    constexpr const char* K = "vectorLocalAddDir";

    const std::string raw_dir = io.str("dir");
    if (raw_dir.empty()) { io.error(K, "dir param is required", 2); return BlockResult::Error; }

    std::error_code ec;
    const fs::path scan_dir = fs::absolute(media::path::path_from_utf8(raw_dir), ec);
    if (ec || !fs::is_directory(scan_dir, ec)) {
        io.error(K, "not a directory: " + raw_dir, 2); return BlockResult::Error;
    }

    const int max_files     = static_cast<int>(io.i64("maxFiles", 500));
    const bool recursive    = io.i64("recursive", 1) != 0;
    const int chunk_size    = static_cast<int>(io.i64("chunkSize",   500));
    const int chunk_overlap = static_cast<int>(io.i64("chunkOverlap", 100));

    std::string dir_abs;
    const auto vs = vl_open(io, K, dir_abs);
    if (!vs) return BlockResult::Error;

    media::llm::ChunkOptions copts;
    copts.max_chars     = chunk_size;
    copts.overlap_chars = chunk_overlap;
    copts.min_chars     = 30;

    J files_arr = J::array();
    int total_chunks = 0;
    int file_count   = 0;

    auto process_entry = [&](const fs::directory_entry& entry) {
        if (file_count >= max_files) return;
        if (!entry.is_regular_file()) return;

        const fs::path& p = entry.path();
        std::string ext   = media::path::path_to_utf8(p.extension());
        ext = media::strutil::lower_ascii(std::move(ext));
        if (!vl_is_supported_ext(ext)) return;

        const std::string file_path = media::path::path_to_utf8(p);
        ++file_count;

        J file_result;
        file_result["file"] = file_path;

        if (vs->has_path(file_path)) {
            file_result["count"]   = 0;
            file_result["skipped"] = true;
            files_arr.push_back(std::move(file_result));
            return;
        }

        const std::wstring ext_w(ext.begin(), ext.end());
        const bool office = pmui::is_office_document_ext(ext_w);
        const bool md     = pmui::is_markdown_ext(ext_w);

        std::vector<media::llm::Chunk> chunks;

        if (office) {
#if defined(FEATURE_OFFICE_EXTRACTOR) && FEATURE_OFFICE_EXTRACTOR
            media::llm::OfficeSourceAdapter adapter;
            for (const auto& unit : adapter.extract(p)) {
                auto uc = media::llm::PlainTextChunker{}.chunk(unit.uri, unit.text, copts);
                for (auto& ch : uc) if (ch.title.empty()) ch.title = unit.title;
                chunks.insert(chunks.end(), std::make_move_iterator(uc.begin()),
                              std::make_move_iterator(uc.end()));
            }
#endif
        } else {
            std::uintmax_t sz = fs::file_size(p, ec);
            if (ec || sz > k_vl_max_file_bytes) { return; }
            std::string text(sz, '\0');
            { std::ifstream f(p, std::ios::binary); f.read(text.data(), static_cast<std::streamsize>(sz)); }
            if (text.find('\0') != std::string::npos) return;

            if (md) {
                chunks = media::llm::MarkdownChunker{}.chunk(file_path, text, copts);
            } else {
                chunks = media::llm::PlainTextChunker{}.chunk(file_path, text, copts);
            }
        }

        int added = 0;
        for (const auto& ch : chunks) {
            media::llm::VStoreDoc doc;
            doc.source = "dir";
            doc.path   = file_path;
            doc.title  = ch.title.empty() ? media::path::path_to_utf8(p.filename()) : ch.title;
            doc.chunk  = ch.content;
            doc.tags   = ch.tags;
            if (vs->add(doc, ch.embedding_text) != 0) ++added;
        }

        total_chunks += added;
        file_result["count"]   = added;
        file_result["skipped"] = false;
        files_arr.push_back(std::move(file_result));
    };

    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(scan_dir, ec))
            process_entry(entry);
    } else {
        for (const auto& entry : fs::directory_iterator(scan_dir, ec))
            process_entry(entry);
    }

    if (total_chunks > 0 && !vs->save()) {
        io.error(K, "save failed: " + vs->error(), 1);
        return BlockResult::Error;
    }

    J result;
    result["files"]       = files_arr;
    result["totalChunks"] = total_chunks;
    result["storePath"]   = dir_abs;
    result["docCount"]    = vs->doc_count();

    io.ok(K, std::to_string(total_chunks) + " chunk(s) from " + std::to_string(file_count) + " file(s)")
      .set("storePath", dir_abs)
      .set("totalChunks", ParamKind::integer_, Value{static_cast<long long>(total_chunks)})
      .set("docCount",    ParamKind::integer_, Value{static_cast<long long>(vs->doc_count())})
      .result(Value::from_json(result), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── vectorLocalDelete ─────────────────────────────────────────────────────────
//
// Remove all chunks for a given source path from the store.
// PREVIOUS = {deleted:N, storePath, docCount}.

BlockResult vector_local_delete_block(BlockIO& io)
{
    constexpr const char* K = "vectorLocalDelete";

    const std::string target = io.str("path");
    if (target.empty()) { io.error(K, "path param is required", 2); return BlockResult::Error; }

    std::string dir_abs;
    const auto vs = vl_open(io, K, dir_abs);
    if (!vs) return BlockResult::Error;

    const std::size_t deleted = vs->remove_path(target);

    if (deleted > 0 && !vs->save()) {
        io.error(K, "save failed: " + vs->error(), 1);
        return BlockResult::Error;
    }

    J result;
    result["deleted"]   = deleted;
    result["path"]      = target;
    result["storePath"] = dir_abs;
    result["docCount"]  = vs->doc_count();

    io.ok(K, std::to_string(deleted) + " chunk(s) deleted for: " + target)
      .set("storePath", dir_abs)
      .set("deleted",  ParamKind::integer_, Value{static_cast<long long>(deleted)})
      .set("docCount", ParamKind::integer_, Value{static_cast<long long>(vs->doc_count())})
      .result(Value::from_json(result), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── vectorLocalStats ──────────────────────────────────────────────────────────
//
// Return metadata about an open store (model, dims, doc count, index size).
// PREVIOUS = {docCount, dims, modelPath, storePath, indexSizeBytes}.

BlockResult vector_local_stats_block(BlockIO& io)
{
    constexpr const char* K = "vectorLocalStats";

    std::string dir_abs;
    const auto vs = vl_open(io, K, dir_abs);
    if (!vs) return BlockResult::Error;

    // Sum store directory size
    std::uintmax_t index_bytes = 0;
    {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(media::path::path_from_utf8(dir_abs), ec))
            if (e.is_regular_file(ec)) index_bytes += fs::file_size(e.path(), ec);
    }

    J result = store_stats_json(*vs);
    result["storePath"]      = dir_abs;
    result["indexSizeBytes"] = index_bytes;

    io.ok(K, std::to_string(vs->doc_count()) + " docs · " + std::to_string(index_bytes / 1024)
             + " KiB" + (vs->gpu() ? " · GPU" : ""))
      .set("storePath",      dir_abs)
      .set("docCount",       ParamKind::integer_, Value{static_cast<long long>(vs->doc_count())})
      .set("indexSizeBytes", ParamKind::integer_, Value{static_cast<long long>(index_bytes)})
      .set("gpu",            ParamKind::boolean_, Value{vs->gpu()})
      .result(Value::from_json(result), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── vectorLocalSearch ─────────────────────────────────────────────────────────
//
// Embed the query locally and return top-k results by cosine similarity.
// PREVIOUS = {hits:[{id,source,path,title,chunk,tags,score},...], count:N}.

BlockResult vector_local_search_block(BlockIO& io)
{
    constexpr const char* K = "vectorLocalSearch";

    const std::string query = io.str("query");
    if (query.empty()) { io.error(K, "query param is required", 2); return BlockResult::Error; }

    std::string dir_abs;
    const auto vs = vl_open(io, K, dir_abs);
    if (!vs) return BlockResult::Error;

    const std::size_t top_k         = static_cast<std::size_t>(io.i64("topK", 10));
    const std::string filter_source = io.str("filterSource");
    const std::string filter_path   = io.str("filterPath");

    const auto hits = vs->search(query, top_k, filter_source, filter_path);

    J hits_arr = J::array();
    for (const auto& h : hits) {
        J item;
        item["id"]     = h.doc.id;
        item["source"] = h.doc.source;
        item["path"]   = h.doc.path;
        item["title"]  = h.doc.title;
        item["chunk"]  = h.doc.chunk;
        item["tags"]   = h.doc.tags;
        item["score"]  = h.score;
        hits_arr.push_back(std::move(item));
    }

    J result;
    result["hits"]      = hits_arr;
    result["count"]     = hits.size();
    result["query"]     = query;
    result["storePath"] = dir_abs;

    io.ok(K, std::to_string(hits.size()) + " hit(s) for: " + query)
      .set("storePath", dir_abs)
      .set("query",     query)
      .set("count",     ParamKind::integer_, Value{static_cast<long long>(hits.size())})
      .result(Value::from_json(result), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── registration ──────────────────────────────────────────────────────────────

} // namespace (anon)

#if defined(FEATURE_VECTOR_LOCAL_XBLOX) && FEATURE_VECTOR_LOCAL_XBLOX

void clear_vector_local_store_registry()
{
    std::lock_guard<std::mutex> lk(g_registry_mu);
    g_registry.clear();
}

#endif

void register_vector_local_blocks(BlockRegistry& registry)
{
#if defined(FEATURE_VECTOR_LOCAL_XBLOX) && FEATURE_VECTOR_LOCAL_XBLOX

    using bdb = BlockDescriptorBuilder;

    // ── vectorLocalOpen ───────────────────────────────────────────────────────
    registry["vectorLocalOpen"] = bdb(
        "vectorLocalOpen", vector_local_open_block,
        "Open Local Vector Store", "Vector",
        "Open or create a local GGUF-backed vector store at storePath. "
        "Loads the embedding model once; subsequent blocks on the same path reuse it. "
        "Set gpuLayers to offload embedding to the GPU (-1 = all layers) for faster ingest. "
        "PREVIOUS = {docCount, dims, modelPath, gpu, gpuLayers}.",
        J{{"kind","vectorLocalOpen"},{"storeAs","storeInfo"}})
        .params([]{
            auto p = vl_common_params();
            p.push_back(vl_store_as("storeInfo"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Store info")
        .output("docCount", ParamKind::integer_, "Document count")
        .output("dims", ParamKind::integer_, "Dimensions")
        .output("gpu", ParamKind::boolean_, "GPU active")
        .exec({false, true, true});

    // ── vectorLocalAddText ────────────────────────────────────────────────────
    registry["vectorLocalAddText"] = bdb(
        "vectorLocalAddText", vector_local_add_text_block,
        "Add Text to Local Vector Store", "Vector",
        "Embed text locally (GGUF model), chunk if needed, insert into the store, and save. "
        "All embedding is offline  -  no network calls. "
        "PREVIOUS = {ids:[...], count:N, storePath, docCount}.",
        J{{"kind","vectorLocalAddText"},{"source","text"},{"chunkSize",500},
          {"chunkOverlap",100},{"storeAs","addResult"}})
        .params([]{
            auto p = vl_common_params();
            p.insert(p.begin(), {
                pd_str("text").in().req().deep().dflt("").desc(
                    "Text to embed and insert. Long text is chunked automatically. Supports ${var}."),
                pd_str("source").in().deep().dflt("text").desc(
                    "Source label for the document (e.g. text, memory, note). Supports ${var}."),
                pd_str("path").in().deep().dflt("").desc(
                    "Origin path or URI (optional). Supports ${var}."),
                pd_str("title").in().deep().dflt("").desc(
                    "Human-readable title (optional). Supports ${var}."),
                pd_str("tags").in().deep().dflt("").desc(
                    "JSON array of tag strings, e.g. [\"docs\",\"2026\"]. Supports ${var}."),
                vl_chunk_size_param(),
                vl_chunk_overlap_param(),
            });
            p.push_back(vl_store_as("addResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Add result")
        .output("count", ParamKind::integer_, "Added count")
        .output("docCount", ParamKind::integer_, "Document count")
        .exec({false, true, true});

    // ── vectorLocalAddFile ────────────────────────────────────────────────────
    registry["vectorLocalAddFile"] = bdb(
        "vectorLocalAddFile", vector_local_add_file_block,
        "Add File to Local Vector Store", "Vector",
        "Read a text file, chunk it, embed each chunk with the local GGUF model, "
        "insert into the store. Binary/image files and files > 1 MiB are rejected. "
        "PREVIOUS = {ids:[...], count:N, file, storePath, docCount}.",
        J{{"kind","vectorLocalAddFile"},{"source","file"},{"chunkSize",500},
          {"chunkOverlap",100},{"storeAs","addResult"}})
        .params([]{
            auto p = vl_common_params();
            p.insert(p.begin(), {
                pd_file("file").in().req().deep().must_exist().guard().dflt("").desc(
                    "Path to a text file to embed and insert. Supports ${var}."),
                pd_str("source").in().deep().dflt("file").desc(
                    "Source label (default: file). Supports ${var}."),
                pd_str("title").in().deep().dflt("").desc(
                    "Override title (default: filename). Supports ${var}."),
                pd_str("tags").in().deep().dflt("").desc(
                    "JSON array of tag strings. Supports ${var}."),
                vl_chunk_size_param(),
                vl_chunk_overlap_param(),
            });
            p.push_back(vl_store_as("addResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Add result")
        .output("count", ParamKind::integer_, "Added count")
        .output("docCount", ParamKind::integer_, "Document count")
        .exec({false, true, true});

    // ── vectorLocalAddDir ─────────────────────────────────────────────────────
    registry["vectorLocalAddDir"] = bdb(
        "vectorLocalAddDir", vector_local_add_dir_block,
        "Index Directory into Local Vector Store", "Vector",
        "Recursively index all supported files (txt, md, docx, xlsx, pptx) in a directory. "
        "Already-indexed paths are skipped. "
        "PREVIOUS = {files:[{file,count,skipped}], totalChunks:N, storePath, docCount}.",
        J{{"kind","vectorLocalAddDir"},{"recursive",true},{"maxFiles",500},
          {"chunkSize",500},{"chunkOverlap",100},{"storeAs","addDirResult"}})
        .params([]{
            auto p = vl_common_params();
            p.insert(p.begin(), {
                pd_dir("dir").in().req().deep().dflt("").desc(
                    "Directory to index. Supports ${var}."),
                pd_bool("recursive").grp("filter").dflt(true).desc(
                    "Descend into subdirectories."),
                pd_int("maxFiles").grp("filter").dflt(500).range(1, 10000).desc(
                    "Hard cap on files processed in one call."),
                vl_chunk_size_param(),
                vl_chunk_overlap_param(),
            });
            p.push_back(vl_store_as("addDirResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Directory result")
        .output("totalChunks", ParamKind::integer_, "Total chunks")
        .output("docCount", ParamKind::integer_, "Document count")
        .exec({false, true, true});

    // ── vectorLocalDelete ─────────────────────────────────────────────────────
    registry["vectorLocalDelete"] = bdb(
        "vectorLocalDelete", vector_local_delete_block,
        "Delete from Local Vector Store by Path", "Vector",
        "Remove all chunks for a given source path from the store. "
        "PREVIOUS = {deleted:N, path, storePath, docCount}.",
        J{{"kind","vectorLocalDelete"},{"storeAs","deleteResult"}})
        .params([]{
            auto p = vl_common_params();
            p.insert(p.begin(), {
                pd_str("path").in().req().deep().dflt("").desc(
                    "Exact source path to remove. Supports ${var}."),
            });
            p.push_back(vl_store_as("deleteResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Delete result")
        .output("deleted", ParamKind::integer_, "Deleted count")
        .output("docCount", ParamKind::integer_, "Document count")
        .exec({false, true, true});

    // ── vectorLocalStats ──────────────────────────────────────────────────────
    registry["vectorLocalStats"] = bdb(
        "vectorLocalStats", vector_local_stats_block,
        "Local Vector Store Stats", "Vector",
        "Return metadata about an open store: doc count, dims, model, index size. "
        "PREVIOUS = {docCount, dims, modelPath, storePath, indexSizeBytes}.",
        J{{"kind","vectorLocalStats"},{"storeAs","storeStats"}})
        .params([]{
            auto p = vl_common_params();
            p.push_back(vl_store_as("storeStats"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Store stats")
        .output("docCount", ParamKind::integer_, "Document count")
        .output("indexSizeBytes", ParamKind::integer_, "Index size bytes")
        .output("gpu", ParamKind::boolean_, "GPU active")
        .exec({false, true, true});

    // ── vectorLocalSearch ─────────────────────────────────────────────────────
    registry["vectorLocalSearch"] = bdb(
        "vectorLocalSearch", vector_local_search_block,
        "Search Local Vector Store", "Vector",
        "Embed the query locally (GGUF model) and return top-k results by cosine similarity. "
        "Offline, no network. "
        "PREVIOUS = {hits:[{id,source,path,title,chunk,tags,score},...], count:N, query}.",
        J{{"kind","vectorLocalSearch"},{"topK",10},{"storeAs","searchResult"}})
        .params([]{
            auto p = vl_common_params();
            p.insert(p.begin(), {
                pd_str("query").in().req().deep().dflt("").desc(
                    "Semantic search query. Supports ${var}."),
                pd_int("topK").grp("filter").dflt(10).range(1, 200).desc(
                    "Maximum number of results to return."),
                pd_str("filterSource").grp("filter").deep().dflt("").desc(
                    "Restrict to docs with this source label (empty = no filter)."),
                pd_str("filterPath").grp("filter").deep().dflt("").desc(
                    "Restrict to docs with this exact path (empty = no filter)."),
            });
            p.push_back(vl_store_as("searchResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Search result")
        .output("count", ParamKind::integer_, "Hit count")
        .exec({false, true, true})
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

#endif // FEATURE_VECTOR_LOCAL_XBLOX  -  registration guard
}

#endif // FEATURE_VECTOR_LOCAL_XBLOX  -  handler implementations

} // namespace media::xblox::blocks
