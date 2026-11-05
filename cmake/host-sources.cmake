# Source list and include root for the XBlox engine.
# Included by the Tanit host and by this package's top-level build.
# Include root is src/. Callers write #include "xblox_commands.hpp"
# and #include "blocks/....hpp".

set(PM_XBLOX_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(PM_XBLOX_SRC "${PM_XBLOX_ROOT}/src")

function(pm_xblox_prepare)
  get_filename_component(_src "${PM_XBLOX_SRC}" ABSOLUTE)
  set(PM_XBLOX_SRC "${_src}" PARENT_SCOPE)
  set(PM_XBLOX_ROOT "${PM_XBLOX_ROOT}" PARENT_SCOPE)
endfunction()

# Engine translation units compiled into the host image (pm-media).
# Session, llm blocks, and jq query stay on the host target that already
# owns them, so a chat plugin and a CLI do not each get a second copy.
function(pm_xblox_core_sources out_var)
  set(_src "${PM_XBLOX_SRC}")
  set(_core
    "${_src}/Scope_Variables.cpp"
    "${_src}/xblox_commands.cpp"
    "${_src}/artifact_ledger.cpp"
    "${_src}/xblox_expressions.cpp"
    "${_src}/xblox_json.cpp"
    "${_src}/xblox_markdown.cpp"
    "${_src}/xblox_diagram_styles.cpp"
    "${_src}/xblox_diagram_pathfinding.cpp"
    "${_src}/xblox_diagram_text.cpp"
    "${_src}/xblox_diagram.cpp"
    "${_src}/xblox_sequence_diagram.cpp"
    "${_src}/xblox_prepared_run.cpp"
    "${_src}/xblox_runtime_control.cpp"
    "${_src}/xblox_strings.cpp"
    "${_src}/blocks/app_blocks.cpp"
    "${_src}/blocks/browser_blocks.cpp"
    "${_src}/blocks/audio_blocks_tts-scripted.cpp"
    "${_src}/blocks/audio_blocks.cpp"
    "${_src}/blocks/audio_blocks_voice_commands.cpp"
    "${_src}/blocks/block_params_mapping.cpp"
    "${_src}/blocks/block_params_ui.cpp"
    "${_src}/blocks/parameter_operations.hpp"
    "${_src}/blocks/json_templates.cpp"
    "${_src}/blocks/bluetooth_blocks.cpp"
    "${_src}/blocks/builtin_blocks.cpp"
    "${_src}/blocks/block_resolver.cpp"
    "${_src}/blocks/data_blocks.cpp"
    "${_src}/blocks/fs_blocks.cpp"
    "${_src}/blocks/fs_blocks_win32.cpp"
    "${_src}/blocks/image_blocks.cpp"
    "${_src}/blocks/input_blocks.cpp"
    "${_src}/blocks/model_blocks.cpp"
    "${_src}/blocks/network_blocks.cpp"
    "${_src}/blocks/ocr_blocks.cpp"
    "${_src}/blocks/shell_blocks.cpp"
    "${_src}/blocks/video_blocks.cpp"
    "${_src}/blocks/video_core_blocks.cpp"
    "${_src}/blocks/video_detect_blocks.cpp"
    "${_src}/blocks/video_blocks_streaming.cpp"
    "${_src}/blocks/video_filter_blocks.cpp"
    "${_src}/blocks/video_vcam_blocks.cpp"
    "${_src}/blocks/vision_blocks.cpp"
    "${_src}/utils/conv.cpp"
    "${_src}/Object.cpp"
    "${_src}/Parameter.cpp"
    "${_src}/ParameterIn.cpp"
    "${_src}/ParameterOut.cpp"
    "${_src}/ParameterManager.cpp"
    "${_src}/Context.cpp"
    "${_src}/Scope.cpp"
  )
  if(FEATURE_MODBUS_XBLOX)
    list(APPEND _core "${_src}/blocks/modbus_blocks.cpp")
  endif()
  if(FEATURE_MQTT_XBLOX)
    list(APPEND _core "${_src}/blocks/mqtt_blocks.cpp")
  endif()
  if(FEATURE_SERVICE_XBLOX)
    list(APPEND _core "${_src}/blocks/service_blocks.cpp")
  endif()
  if(FEATURE_VECTOR_XBLOX)
    list(APPEND _core "${_src}/blocks/vector_blocks.cpp")
  endif()
  if(FEATURE_VECTOR_LOCAL_XBLOX)
    list(APPEND _core "${_src}/blocks/vector_blocks_local.cpp")
  endif()
  if(FEATURE_MCP_CLIENT)
    list(APPEND _core "${_src}/blocks/network_blocks_mcp.cpp")
  endif()
  set(${out_var} ${_core} PARENT_SCOPE)
endfunction()

set(PM_XBLOX_JSON_QUERY "${PM_XBLOX_SRC}/blocks/json_query.cpp")
set(PM_XBLOX_LLM_BLOCKS "${PM_XBLOX_SRC}/blocks/llm_blocks.cpp")
set(PM_XBLOX_SESSION "${PM_XBLOX_SRC}/xblox_session.cpp")
set(PM_XBLOX_DIAGRAM "${PM_XBLOX_SRC}/xblox_diagram.cpp")
