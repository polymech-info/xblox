// XBlox filesystem blocks  -  fsRead, fsWrite, fsAppend, fsExists,
//                           fsHash, fsDelete, fsList, fsMkdir, fsNew,
//                           fsCopy, fsMove.
//
// fsCopy / fsMove / fsMkdir go through pm::vfs::ops::run_transfer (URI
// queue). fsWrite stays a local content write. fsNew is Win32 Shell New.
//
// Every handler follows the PREVIOUS/storeAs contract used by the rest of
// the runtime:
//   - The block's "storeAs" field (string) names a context variable that
//     receives the primary result. If absent the result goes to PREVIOUS only.
//   - Blocks that derive their *path* from PREVIOUS accept an empty "path"
//     field and pick up the current PREVIOUS string as the path.

#include "blocks/fs_blocks.hpp"
#include "blocks/block_variables.hpp"

#include "logging/logger.h"
#include "vfs/operations/transfer.hpp"

#include <picosha2.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::xblox::blocks {
namespace {

// ── helpers ───────────────────────────────────────────────────────────────────
//
// Inputs are sourced + interpolated + precondition-checked by the executor's
// resolver: `io.str("path")` yields the final ${var}-expanded path (block field
// or, for `.prev()` params, the PREVIOUS value). Existence/writability are
// declared as ParamConstraints on the schema, so handlers no longer re-check.
static std::string fs_path(BlockIO& io)
{
    return io.str("path");
}

// Write `data` to `path` atomically: write to a sibling temp file then rename
// over the target. std::filesystem::rename is atomic on POSIX (rename(2)) and
// on Windows MSVC calls MoveFileExW(MOVEFILE_REPLACE_EXISTING) internally.
// The temp sits in the same directory so the rename is always same-volume.
static bool atomic_write(const std::string& path, const std::string& data, std::string& err)
{
    const std::filesystem::path target = media::path::path_from_utf8(path);
    std::filesystem::path tmp = target;
    tmp += media::path::path_from_utf8(".tmp~");
    {
        std::ofstream f(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot open tmp: " + media::path::path_to_utf8(tmp); return false; }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) { err = "write failed: " + media::path::path_to_utf8(tmp); return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        err = "rename failed: " + ec.message();
        return false;
    }
    return true;
}

// ── fsRead ────────────────────────────────────────────────────────────────────

bool fs_read_block(BlockIO& io)
{
    // path is declared `.must_exist()`, so the resolver already failed the block
    // with a precondition error if it was empty or missing  -  we get a real file.
    const std::string file_path = fs_path(io);
    const std::string encoding  = io.str("encoding"); // "text" | "json"; default text

    std::ifstream f(media::path::path_from_utf8(file_path), std::ios::binary);
    if (!f) {
        io.error("fsRead", "fsRead: cannot open: " + file_path, 1);
        return true;
    }
    std::string content((std::istreambuf_iterator<char>(f)), {});

    Value result{content};
    if (encoding == "json") {
        try {
            result = Value::from_json(nlohmann::json::parse(content));
        } catch (...) {
            io.error("fsRead", "fsRead: JSON parse error in: " + file_path, 1);
            return true;
        }
    }

    io.ok("fsRead", "file read: " + file_path)
      .set("path", file_path)
      .set("size", static_cast<long long>(content.size()))
      .result(result, encoding == "json" ? ParamKind::json_value : ParamKind::string_);
    return true;
}

// ── fsHash ────────────────────────────────────────────────────────────────────

bool fs_hash_block(BlockIO& io)
{
    const std::string file_path = fs_path(io);
    const std::string algorithm = io.str("algorithm", "sha256");
    const int         prefix    = static_cast<int>(std::clamp<long long>(io.i64("prefixLength", 0), 0, 64));
    if (algorithm != "sha256") {
        io.error("fsHash", "fsHash: unsupported algorithm: " + algorithm, 2)
          .set("path", file_path);
        return true;
    }

    std::ifstream f(media::path::path_from_utf8(file_path), std::ios::binary);
    if (!f) {
        io.error("fsHash", "fsHash: cannot open: " + file_path, 1);
        return true;
    }
    const std::string content((std::istreambuf_iterator<char>(f)), {});
    std::string hex;
    picosha2::hash256_hex_string(content.begin(), content.end(), hex);
    const std::string result = prefix > 0 ? hex.substr(0, static_cast<std::size_t>(prefix)) : hex;

    io.ok("fsHash", "file hashed: " + file_path)
      .set("path", file_path)
      .set("algorithm", algorithm)
      .set("sha256", hex)
      .set("prefixLength", prefix)
      .set("size", static_cast<long long>(content.size()))
      .result(result, ParamKind::string_);
    return true;
}

// ── fsWrite ───────────────────────────────────────────────────────────────────
// Content priority: block["content"] -> PREVIOUS string/JSON.
// Result stored in PREVIOUS / storeAs: the file path (string).
//
// JSON-aware append: when append=true and both the existing file content and the
// incoming content parse as JSON, the merge follows the existing root type:
//   array  -> incoming array elements are spliced in; any other value is push_back'd.
//   object -> incoming object keys are merged (shallow, incoming wins on collision).
// Falls back to raw byte-append if either side fails to parse as JSON.

bool fs_write_block(BlockIO& io)
{
    // path is declared `.writable().create_parents()`: the resolver rejected an
    // empty path and already created the parent directory tree (except under
    // --simulate, where the write is suppressed anyway). `content` is `.prev()`,
    // so io.str() falls back to PREVIOUS; both are already ${var}-interpolated.
    const std::string file_path = fs_path(io);
    const bool        append    = io.flag("append", false);
    const std::string content   = io.str("content");

    // JSON-aware append: read existing file, merge, atomic-write back.
    if (append) {
        std::ifstream rf(media::path::path_from_utf8(file_path), std::ios::binary);
        if (rf) {
            const std::string existing((std::istreambuf_iterator<char>(rf)), {});
            rf.close();
            try {
                auto jexist = nlohmann::json::parse(existing);
                auto jnew   = nlohmann::json::parse(content);
                if (jexist.is_array()) {
                    if (jnew.is_array())
                        for (auto& el : jnew) jexist.push_back(std::move(el));
                    else
                        jexist.push_back(std::move(jnew));
                } else if (jexist.is_object() && jnew.is_object()) {
                    jexist.merge_patch(jnew);
                } else {
                    goto raw_append;
                }
                const std::string merged = jexist.dump(2);
                std::string err;
                if (!atomic_write(file_path, merged, err)) {
                    io.error("fsWrite", "fsWrite: " + err, 1);
                    return true;
                }
                io.ok("fsWrite", "file json-appended: " + file_path)
                  .set("path", file_path)
                  .set("size", static_cast<long long>(merged.size()))
                  .set("append", true)
                  .result(file_path, ParamKind::file_path);
                return true;
            } catch (...) {
                // Not valid JSON on either side  -  fall through to raw append.
            }
        }
    }

raw_append:
    if (append) {
        // Raw byte-append: not a swap operation, keep as direct write.
        std::ofstream f(media::path::path_from_utf8(file_path), std::ios::out | std::ios::binary | std::ios::app);
        if (!f) {
            io.error("fsWrite", "fsWrite: cannot write: " + file_path, 1);
            return true;
        }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    } else {
        // Overwrite: atomic write-to-temp + rename.
        std::string err;
        if (!atomic_write(file_path, content, err)) {
            io.error("fsWrite", "fsWrite: " + err, 1);
            return true;
        }
    }

    io.ok("fsWrite", (append ? "file appended: " : "file written: ") + file_path)
      .set("path", file_path)
      .set("size", static_cast<long long>(content.size()))
      .set("append", append)
      .result(file_path, ParamKind::file_path);
    return true;
}

// ── fsExists ──────────────────────────────────────────────────────────────────

bool fs_exists_block(BlockIO& io)
{
    const std::string file_path = fs_path(io);

    std::error_code ec;
    const bool exists = !file_path.empty() && std::filesystem::exists(file_path, ec);

    io.ok("fsExists", exists ? ("exists: " + file_path) : ("not found: " + file_path))
      .set("path", file_path)
      .set("exists", exists)
      .result(exists, ParamKind::boolean_);
    return true;
}

// ── fsDelete ──────────────────────────────────────────────────────────────────

bool fs_delete_block(BlockIO& io)
{
    const std::string file_path = fs_path(io);
    const bool        recursive = io.flag("recursive", false);

    if (file_path.empty()) {
        io.error("fsDelete", "fsDelete: path is empty", 2);
        return true;
    }

    std::error_code ec;
    bool deleted = false;
    if (recursive)
        deleted = std::filesystem::remove_all(file_path, ec) > 0;
    else
        deleted = std::filesystem::remove(file_path, ec);

    if (ec) {
        io.error("fsDelete", "fsDelete failed: " + ec.message(), 1);
        return true;
    }

    io.ok("fsDelete", deleted ? "deleted: " + file_path : "not found: " + file_path)
      .set("path", file_path)
      .set("deleted", deleted)
      .result(deleted, ParamKind::boolean_);
    return true;
}

// ── fsList ────────────────────────────────────────────────────────────────────
// Result stored in PREVIOUS / storeAs: JSON array of {name, path, type, size?}.
//
// The `path` param composes resolution features (.globs()): when it carries a
// glob pattern the resolver expands it to an *array* of matched paths and this
// handler stats each one; otherwise it's a plain directory we enumerate. The
// `only` enum (all|dir|file) filters entries  -  `only:"dir"` lists folders only.

bool fs_list_block(BlockIO& io)
{
    std::string only = io.str("only", "all");
    only = media::strutil::lower_ascii(std::move(only));
    const auto keep = [&](bool is_dir) {
        return only == "all" || (only == "dir" && is_dir) || (only == "file" && !is_dir);
    };

    std::error_code ec;
    nlohmann::json entries = nlohmann::json::array();
    const auto add_entry = [&](const std::filesystem::path& p) {
        const bool is_dir = std::filesystem::is_directory(p, ec);
        if (!keep(is_dir)) return;
        nlohmann::json e;
        e["name"] = media::path::path_to_utf8(p.filename());
        e["path"] = media::path::path_to_utf8(p);
        e["type"] = is_dir ? "dir" : "file";
        if (std::filesystem::is_regular_file(p, ec)) {
            const auto sz = std::filesystem::file_size(p, ec);
            if (!ec) e["size"] = static_cast<long long>(sz);
        }
        entries.push_back(std::move(e));
    };

    std::string source;
    // Glob mode: the resolver already expanded the pattern to a list of paths.
    if (const Value* pv = io.value("path"); pv && pv->is_array()) {
        const nlohmann::json arr = pv->to_json();
        source = "glob (" + std::to_string(arr.size()) + " match(es))";
        for (const auto& el : arr)
            if (el.is_string()) add_entry(media::path::path_from_utf8(el.get<std::string>()));
    } else {
        // Directory mode: enumerate children of a plain (interpolated) path.
        const std::string dir_path = fs_path(io);
        if (dir_path.empty()) {
            io.error("fsList", "fsList: path is empty", 2);
            return true;
        }
        if (!std::filesystem::is_directory(dir_path, ec)) {
            io.error("fsList", "fsList: not a directory: " + dir_path, 1);
            return true;
        }
        source = dir_path;
        for (std::filesystem::directory_iterator it(dir_path, ec), end; !ec && it != end; it.increment(ec))
            add_entry(it->path());
    }

    const auto count = static_cast<long long>(entries.size());
    io.ok("fsList", "listed " + std::to_string(count) + " entries in " + source)
      .set("path", source)
      .set("count", count)
      .result(Value::from_json(std::move(entries)), ParamKind::json_value);
    return true;
}

// ── fsCopy / fsMove / fsMkdir ─────────────────────────────────────────────────
// Paths stay as `${…}` templates on the block; the VFS transfer lib expands
// them (same map as command_variables + xblox context extras).

void append_transfer_spec(std::vector<std::string>& out, const nlohmann::json& v)
{
    if (v.is_null())
        return;
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s.empty())
            return;
        if (s.front() == '[') {
            try {
                append_transfer_spec(out, nlohmann::json::parse(s));
                return;
            } catch (...) {
            }
        }
        out.push_back(s);
        return;
    }
    if (v.is_array()) {
        for (const auto& el : v)
            append_transfer_spec(out, el);
        return;
    }
    if (v.is_object() && v.contains("path") && v["path"].is_string())
        out.push_back(v["path"].get<std::string>());
}

void split_filter_globs(const std::string& raw, std::vector<std::string>& out)
{
    std::string cur;
    for (char c : raw) {
        if (c == ';' || c == '\n') {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
}

std::vector<std::string> fs_transfer_sources(BlockIO& io)
{
    std::vector<std::string> out;
    if (const Value* v = io.value("src"))
        append_transfer_spec(out, v->to_json());
    if (out.empty() && io.runtime().context.is_object()
        && io.runtime().context.contains("PREVIOUS")) {
        append_transfer_spec(out, io.runtime().context["PREVIOUS"]);
    }
    return out;
}

std::vector<std::string> fs_create_targets(BlockIO& io)
{
    std::vector<std::string> out;
    if (const Value* v = io.value("path"))
        append_transfer_spec(out, v->to_json());
    if (out.empty() && io.runtime().context.is_object()
        && io.runtime().context.contains("PREVIOUS")) {
        append_transfer_spec(out, io.runtime().context["PREVIOUS"]);
    }
    return out;
}

media::commands::VariableMap xblox_transfer_extra(const BlockRuntime& rt)
{
    media::commands::VariableMap extra;
    if (!rt.context.is_object())
        return extra;
    for (auto it = rt.context.begin(); it != rt.context.end(); ++it) {
        const auto& val = it.value();
        extra.insert_or_assign(
            it.key(),
            (val.is_object() || val.is_array())
                ? val.dump()
                : bv::detail::json_to_string(val));
    }
    return extra;
}

bool fs_transfer_block(BlockIO& io, pm::vfs::ops::Op op, const char* kind)
{
    pm::vfs::ops::TransferRequest req;
    req.op = op;
    req.mode = pm::vfs::ops::ConflictMode::Auto;
    req.strategy = pm::vfs::ops::ConflictStrategy::Error;
    req.sources = fs_transfer_sources(io);
    req.dest = io.str("dst");
    if (req.dest.empty())
        req.dest = io.str("dest");
    split_filter_globs(io.str("include"), req.filter.include);
    split_filter_globs(io.str("exclude"), req.filter.exclude);
    req.context = io.runtime().options.command_context;
    if (req.context.cwd.empty() && !io.runtime().options.default_cwd.empty())
        req.context.cwd = media::path::path_to_utf8(io.runtime().options.default_cwd);
    req.extra = xblox_transfer_extra(io.runtime());

    std::string strategy_err;
    const std::string conflict = io.str("conflict");
    if (!conflict.empty()
        && !pm::vfs::ops::parse_conflict_strategy(conflict, req.strategy, strategy_err)) {
        io.error(kind, std::string(kind) + ": " + strategy_err, 2);
        return true;
    }

    if (req.sources.empty()) {
        io.error(kind, std::string(kind) + ": missing source (src or PREVIOUS)", 2);
        return true;
    }
    if (req.dest.empty()) {
        io.error(kind, std::string(kind) + ": missing destination folder (dst)", 2);
        return true;
    }

    const bool dry_run = io.flag("dryRun", false);
    const pm::vfs::ops::TransferReport report = dry_run
        ? pm::vfs::ops::plan_transfer(std::move(req))
        : pm::vfs::ops::run_transfer(std::move(req));

    if (!report.ok) {
        io.error(kind, report.err.empty() ? (std::string(kind) + " failed") : report.err, 1)
          .set("dest", report.dest)
          .set("dryRun", dry_run);
        return true;
    }

    const auto count = static_cast<long long>(report.sources.size());
    io.ok(kind, std::string(kind) + ": " + std::to_string(count) + " -> " + report.dest)
      .set("dest", report.dest)
      .set("count", count)
      .set("dryRun", report.dry_run)
      .result(report.dest, ParamKind::dir_path);
    return true;
}

bool fs_copy_block(BlockIO& io)
{
    return fs_transfer_block(io, pm::vfs::ops::Op::Copy, "fsCopy");
}

bool fs_move_block(BlockIO& io)
{
    return fs_transfer_block(io, pm::vfs::ops::Op::Move, "fsMove");
}

bool fs_mkdir_block(BlockIO& io)
{
    pm::vfs::ops::TransferRequest req;
    req.op = pm::vfs::ops::Op::Mkdir;
    req.mode = pm::vfs::ops::ConflictMode::Auto;
    req.strategy = pm::vfs::ops::ConflictStrategy::Error;
    req.sources = fs_create_targets(io);
    req.context = io.runtime().options.command_context;
    if (req.context.cwd.empty() && !io.runtime().options.default_cwd.empty())
        req.context.cwd = media::path::path_to_utf8(io.runtime().options.default_cwd);
    req.extra = xblox_transfer_extra(io.runtime());

    if (req.sources.empty()) {
        io.error("fsMkdir", "fsMkdir: path is empty", 2);
        return true;
    }

    const bool dry_run = io.flag("dryRun", false);
    const pm::vfs::ops::TransferReport report = dry_run
        ? pm::vfs::ops::plan_transfer(std::move(req))
        : pm::vfs::ops::run_transfer(std::move(req));

    if (!report.ok) {
        io.error("fsMkdir", report.err.empty() ? "fsMkdir failed" : report.err, 1)
          .set("path", report.dest)
          .set("dryRun", dry_run);
        return true;
    }

    const auto count = static_cast<long long>(report.sources.size());
    io.ok("fsMkdir", "fsMkdir: " + report.dest)
      .set("path", report.dest)
      .set("count", count)
      .set("dryRun", report.dry_run)
      .result(report.dest, ParamKind::dir_path);
    return true;
}

} // namespace

// ── registration ──────────────────────────────────────────────────────────────

void register_fs_blocks(BlockRegistry& registry)
{
    registry["fsRead"] = bdb(
        "fsRead", fs_read_block,
        "Read File", "Files",
        "Read a text or JSON file. Stores file content in PREVIOUS / storeAs.",
        {{"kind","fsRead"}, {"path",""}, {"encoding","text"}, {"storeAs","fileContent"}})
        .params({
            pd_file("path").in().lbl("File").req().prev().must_exist().guard()
                .desc("File to read. Leave empty to use PREVIOUS as the path."),
            pd_enum("encoding", {"text","json"}).grp("read_options").dflt("text")
                .desc("Read as plain text, or parse the file as JSON."),
            pd_store_as().dflt("fileContent"),
        })
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::string_, ParamKind::json_value},
            "File content")
        // Optional pipeline children: run after the read with PREVIOUS = content.
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    registry["fsWrite"] = bdb(
        "fsWrite", fs_write_block,
        "Write File", "Files",
        "Write text from the content field or PREVIOUS to a file. Creates parent folders automatically.",
        {{"kind","fsWrite"}, {"path",""}, {"content",""}, {"append",false}, {"storeAs",""}})
        .params({
            pd_outpath("path").in().lbl("File").req().writable().create_parents().guard()
                .desc("File to write."),
            pd_str("content").in().lbl("Content").prev().dflt("")
                .desc("Content to write. Leave empty to use PREVIOUS."),
            pd_bool("append").grp("write_options").dflt(false)
                .desc("Append to the existing file instead of overwriting it."),
            pd_store_as(),
        })
        .primary_output(ParamKind::file_path, "Written file")
        // Optional pipeline children: run after the write with PREVIOUS = path.
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    registry["fsHash"] = bdb(
        "fsHash", fs_hash_block,
        "Hash File", "Files",
        "Compute a file content hash. Stores the hex digest in PREVIOUS / storeAs.",
        {{"kind","fsHash"}, {"path",""}, {"algorithm","sha256"}, {"prefixLength",0}, {"storeAs","hash"}})
        .params({
            pd_file("path").in().lbl("File").req().prev().must_exist().guard()
                .desc("File to hash. Leave empty to use PREVIOUS as the path."),
            pd_enum("algorithm", {"sha256"}).grp("hash_options").dflt("sha256")
                .desc("Hash algorithm."),
            pd_int("prefixLength").grp("hash_options").dflt(0).range(0, 64)
                .desc("Return only the first N hex characters. 0 returns the full digest."),
            pd_store_as().dflt("hash"),
        })
        .primary_output(ParamKind::string_, "Digest");

    registry["fsExists"] = bdb(
        "fsExists", fs_exists_block,
        "Path Exists", "Files",
        "Check whether a path exists. Stores a boolean in PREVIOUS / storeAs.",
        {{"kind","fsExists"}, {"path",""}, {"storeAs","exists"}})
        .params({
            pd_file("path").in().lbl("Path").req().prev().guard()
                .desc("File or folder to check. Leave empty to use PREVIOUS."),
            pd_store_as().dflt("exists"),
        })
        .primary_output(ParamKind::boolean_, "Exists");

    registry["fsDelete"] = bdb(
        "fsDelete", fs_delete_block,
        "Delete Path", "Files",
        "Delete a file or directory. Returns true if something was deleted.",
        {{"kind","fsDelete"}, {"path",""}, {"recursive",false}, {"storeAs",""}})
        .params({
            pd_file("path").in().lbl("Path").req().prev().guard()
                .desc("File or folder to delete. Leave empty to use PREVIOUS."),
            pd_bool("recursive").grp("delete_options").dflt(false)
                .desc("Delete a directory and all of its contents."),
            pd_store_as(),
        })
        .primary_output(ParamKind::boolean_, "Deleted");

    registry["fsList"] = bdb(
        "fsList", fs_list_block,
        "List Files", "Files",
        "List a directory or glob. Stores an array of {name, path, type, size} in PREVIOUS / storeAs.",
        {{"kind","fsList"}, {"path",""}, {"only","all"}, {"storeAs","entries"}})
        .params({
            pd_dir("path").in().lbl("Directory or glob").req().prev().globs().guard()
                .desc("Directory to list, or a glob pattern such as ${ENV:HOME}/*.png. Leave empty to use PREVIOUS."),
            pd_enum("only", {"all","dir","file"}).grp("list_options").lbl("Show").dflt("all")
                .desc("Show all entries, only folders, or only files."),
            pd_store_as().dflt("entries"),
        })
        .primary_output(ParamKind::json_value, "Entries")
        // Iterable producer: when children are attached, the executor runs them
        // once per listed entry (PREVIOUS = the {name,path,type,size} object,
        // plus a numeric `index`). Use a child Parse(.path) to pull a field.
        .children({{"items", "For each", "body", {"*"}, 0, -1}})
        .iterable();

    registry["fsMkdir"] = bdb(
        "fsMkdir", fs_mkdir_block,
        "Create Folder", "Files",
        "Create a folder through the VFS queue (local, ssh://, ftp://, vfs://). Existing folders succeed. path accepts ${CWD}, ${KNOWNFOLDER:…}, ${PREVIOUS}.",
        {{"kind","fsMkdir"}, {"path",""}, {"dryRun",false}, {"storeAs",""}})
        .params({
            pd_dir("path").in().lbl("Folder").req().prev().guard()
                .placeholder("${CWD}/new-folder")
                .desc("Folder path or URI to create (and parents). Leave empty to use PREVIOUS. ${…} expand in the transfer lib."),
            pd_bool("dryRun").opt().dflt(false)
                .desc("Expand variables only; do not create."),
            pd_store_as(),
        })
        .primary_output(ParamKind::dir_path, "Directory")
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    registry["fsNew"] = bdb(
        "fsNew", fs_new_block,
        "New File", "Files",
        "Create a new file with the Explorer New name for an extension (localized). Stores the absolute path in PREVIOUS / storeAs. Windows only.",
        {{"kind","fsNew"}, {"path",""}, {"ext","txt"}, {"name",""}, {"storeAs",""}})
        .params({
            pd_str("ext").in().lbl("Extension").req().dflt("txt")
                .desc("File type to create, e.g. txt or .txt. Uses the localized Explorer New name for that type. Alias: extension."),
            pd_dir("path").in().lbl("Folder").prev()
                .desc("Folder to create the file in. Leave empty to use PREVIOUS (folder, or parent of a file) or the current directory."),
            pd_str("name").opt().lbl("Name").dflt("")
                .desc("Optional file name override. Empty = localized Explorer New name."),
            pd_store_as(),
        })
        .primary_output(ParamKind::file_path, "Created file")
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    registry["fsCopy"] = bdb(
        "fsCopy", fs_copy_block,
        "Copy Files", "Files",
        "Copy files or folders through the VFS queue (local, ssh://, ftp://, vfs://). src and dst accept ${CWD}, ${KNOWNFOLDER:…}, ${PREVIOUS}.",
        {{"kind","fsCopy"}, {"src",""}, {"dst",""}, {"conflict","error"}, {"dryRun",false}, {"storeAs",""}})
        .params({
            pd_file("src").in().lbl("Source").prev()
                .placeholder("${CWD}/*")
                .desc("File, folder, glob, or URI. Leave empty to use PREVIOUS (path, list, or fsList entries). ${…} expand in the transfer lib."),
            pd_dir("dst").in().lbl("Destination").req()
                .placeholder("${KNOWNFOLDER:Desktop}")
                .desc("Destination folder (path or URI). Keep ${KNOWNFOLDER:Desktop} / ${CWD} as stored templates."),
            pd_enum("conflict", {"error","overwrite","skip","rename","if-newer"}).opt().dflt("error")
                .desc("When a dest file already exists."),
            pd_str("include").opt().dflt("")
                .desc("Keep sources matching this glob. Separate multiple with ;"),
            pd_str("exclude").opt().dflt("")
                .desc("Drop sources matching this glob. Separate multiple with ;"),
            pd_bool("dryRun").opt().dflt(false)
                .desc("Expand variables and sources only; do not copy."),
            pd_store_as(),
        })
        .primary_output(ParamKind::dir_path, "Destination")
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    registry["fsMove"] = bdb(
        "fsMove", fs_move_block,
        "Move Files", "Files",
        "Move files or folders through the VFS queue. Same src/dst ${…} variables as fsCopy.",
        {{"kind","fsMove"}, {"src",""}, {"dst",""}, {"conflict","error"}, {"dryRun",false}, {"storeAs",""}})
        .params({
            pd_file("src").in().lbl("Source").prev()
                .placeholder("${CWD}/*")
                .desc("File, folder, glob, or URI. Leave empty to use PREVIOUS."),
            pd_dir("dst").in().lbl("Destination").req()
                .placeholder("${KNOWNFOLDER:Desktop}")
                .desc("Destination folder (path or URI). Same ${…} templates as fsCopy."),
            pd_enum("conflict", {"error","overwrite","skip","rename","if-newer"}).opt().dflt("error")
                .desc("When a dest file already exists."),
            pd_str("include").opt().dflt("")
                .desc("Keep sources matching this glob. Separate multiple with ;"),
            pd_str("exclude").opt().dflt("")
                .desc("Drop sources matching this glob. Separate multiple with ;"),
            pd_bool("dryRun").opt().dflt(false)
                .desc("Expand variables and sources only; do not move."),
            pd_store_as(),
        })
        .primary_output(ParamKind::dir_path, "Destination")
        .children({{"items", "Then", "body", {"*"}, 0, -1}});
}

} // namespace media::xblox::blocks
