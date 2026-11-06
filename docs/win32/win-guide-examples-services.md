# Back to Win32: Service Surface Examples

These excerpts show REST, MCP, and local service surfaces as thin wrappers around the same native core.

Source files:

- `src/cli/pm_image_cmd_serve.cpp`
- `src/cli/pm_image_mcp_embed.cpp`
- `src/cli/pm_image_cmd_login.cpp`
- `src/win/pixlwiz_login_spawn.cpp`
- `docs/zitadel.md`

## CLI Command Starts REST

```cpp
int pm_image_cmd_serve(CLI::App& app, PmImageCliState& st) {
    media::CacheServerDefaults cd;
    cd.enabled = !st.serve_no_cache;
    cd.cache_dir = st.serve_cache_dir;
    return media::http::run_server(st.host, st.port, cd);
}
```

## Embedded MCP / LLM HTTP Routes

```cpp
static void register_llm_routes(httplib::Server& svr)
{
    register_mcp_streamable_http(svr);

    svr.Get("/v1/llm/tools/list", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(media::llm::tool_catalog_json().dump(), "application/json");
    });

    svr.Post("/v1/llm/tools/call", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(R"json({"ok":false,"error":"invalid JSON body"})json", "application/json");
            return;
        }
        if (!body.contains("name") || !body["name"].is_string()) {
            res.status = 400;
            res.set_content(R"json({"ok":false,"error":"'name' (string) is required"})json", "application/json");
            return;
        }
        const std::string          name = body["name"].get<std::string>();
        const nlohmann::json       args =
            body.contains("arguments") && body["arguments"].is_object() ? body["arguments"]
                                                                       : nlohmann::json::object();
        const media::llm::ExecuteResult out = media::llm::execute(name, args);
        if (!out.ok)
            res.status = 200;
        res.set_content(out.envelope.dump(), "application/json");
    });
}
```

## Environment Override for Automation

```cpp
static void apply_pm_image_mcp_env_override(bool* enabled)
{
    const char* v = std::getenv("PM_IMAGE_MCP");
    if (!v || v[0] == '\0')
        return;
    std::string s(v);
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "0" || s == "false" || s == "off" || s == "no")
        *enabled = false;
    else if (s == "1" || s == "true" || s == "on" || s == "yes")
        *enabled = true;
}
```

## ZITADEL PKCE Verifier

```cpp
/** RFC 7636 code_verifier: 43-128 chars from [A-Z] [a-z] [0-9] "-" "." "_" */
std::string make_code_verifier() {
  static const char cs[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  constexpr size_t k_len = 64;
  std::string out;
  out.resize(k_len);
  std::vector<unsigned char> rnd(k_len);
  if (!random_bytes(rnd.data(), rnd.size())) {
    zlog("WARNING: OS RNG failed; falling back to std::random_device (weaker) "
         "for code_verifier.");
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned> dis(0, 255);
    for (size_t i = 0; i < k_len; ++i)
      rnd[i] = static_cast<unsigned char>(dis(gen));
  }
  for (size_t i = 0; i < k_len; ++i)
    out[i] = cs[rnd[i] % (sizeof(cs) - 1)];
  return out;
}

std::string make_code_challenge_s256(const std::string &verifier) {
  std::vector<unsigned char> hash(picosha2::k_digest_size);
  picosha2::hash256(verifier.begin(), verifier.end(), hash.begin(), hash.end());
  return base64url_encode_bytes(hash.data(), hash.size(), true);
}
```

## Loopback Browser Login

```cpp
const std::string verifier = make_code_verifier();
const std::string challenge = make_code_challenge_s256(verifier);
const std::string state = random_state();

httplib::Server svr;
std::promise<std::string> code_promise;
std::future<std::string> code_future = code_promise.get_future();
std::once_flag code_delivered;

svr.Get("/callback", [&](const httplib::Request &req,
                         httplib::Response &res) {
  const std::string q_state = req.get_param_value("state");
  const std::string code = req.get_param_value("code");
  const std::string oerr = req.get_param_value("error");
  if (!oerr.empty() || q_state != state || code.empty()) {
    std::call_once(code_delivered, [&code_promise] {
      code_promise.set_value(std::string());
    });
    res.set_content("<html><body>Login failed. You can close this tab.</body></html>",
                    "text/html");
    return;
  }
  std::call_once(code_delivered,
                 [&code_promise, code] { code_promise.set_value(code); });
  res.set_content("<html><body>Sign-in complete. You can close this tab and "
                  "return to the terminal.</body></html>",
                  "text/html");
});

int listen_port = 0;
for (int p = port_first; p <= port_last; ++p) {
  if (svr.bind_to_port("127.0.0.1", p)) {
    listen_port = p;
    break;
  }
}
const std::string redirect_uri =
    "http://127.0.0.1:" + std::to_string(listen_port) + "/callback";
```

## Win32 Menu Starts the Same CLI Login

```cpp
STARTUPINFOW si{};
si.cb            = sizeof(si);
si.dwFlags       = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
si.wShowWindow   = SW_HIDE;
si.hStdInput     = h_nul_in;
si.hStdOutput    = h_log_write;
si.hStdError     = h_log_write;

PROCESS_INFORMATION pi{};
const BOOL ok =
    ::CreateProcessW(exe_path.c_str(), cmd_buf.data(), nullptr, nullptr, TRUE, PM_IMAGE_CREATE_NO_WINDOW, nullptr,
                     work_dir.empty() ? nullptr : work_dir.c_str(), &si, &pi);

if (!ok) {
    payload->win32_error = ::GetLastError();
    payload->spawn_error = L"CreateProcessW failed.";
    ::PostMessageW(frame_hwnd, UWM_PIXLWIZ_LOGIN_DONE, 0, reinterpret_cast<LPARAM>(payload));
    return;
}
```
