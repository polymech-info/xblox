#include "blocks/block_resolver.hpp"

namespace media::xblox::blocks {

const ParamDef* BlockIO::def(const char* name) const {
    if (!rt_.param_defs || !name) return nullptr;
    for (const ParamDef& d : *rt_.param_defs)
        if (d.name == name) return &d;
    return nullptr;
}

const nlohmann::json* BlockIO::resolved(const char* name) const {
    if (const ParamDef* d = def(name)) return source_param(block_, *d, rt_);
    const nlohmann::json* f = field(name);
    return (f && !f->is_null()) ? f : nullptr;
}

} // namespace media::xblox::blocks
