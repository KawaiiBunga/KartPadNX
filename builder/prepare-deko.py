#!/usr/bin/env python3
"""Prepare the native GPU dependency. Never stages or resets the game runtime."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
def edit(path, old, new):
    p = root / path
    s = p.read_text()
    if new in s:
        return
    if old not in s:
        raise SystemExit(f"Missing expected context in {p}: {old[:100]}")
    p.write_text(s.replace(old, new, 1), newline="\n")

def insert_unless(path, marker, text, present):
    if present not in (root/path).read_text():
        edit(path, marker, marker+text)

insert_unless('ref/dawn/src/tint/lang/glsl/writer/common/options.h',
              'std::optional<RangeOffsets> depth_range_offsets;',
              '\n    bool depth_zero_to_one = false;\n    bool disable_position_y_negation = false;',
              'bool depth_zero_to_one')
insert_unless('ref/dawn/src/tint/lang/glsl/writer/common/options.h',
              '                 depth_range_offsets,',
              '\n                 depth_zero_to_one,\n                 disable_position_y_negation,',
              '                 disable_position_y_negation,')
insert_unless('ref/dawn/src/tint/lang/glsl/writer/raise/shader_io.h',
              'std::unordered_set<uint32_t> bgra_swizzle_locations{};',
              '\n    bool depth_zero_to_one = false;\n    bool disable_position_y_negation = false;',
              'bool depth_zero_to_one')
edit('ref/dawn/src/tint/lang/glsl/writer/raise/shader_io.cc',
     'auto* new_y = builder.Negation(y);',
     'core::ir::Value* new_y = y;\n            if (!config.disable_position_y_negation) {\n                new_y = builder.Negation(y);\n            }')

edit('ref/dawn/src/tint/lang/glsl/writer/raise/raise.cc',
     'raise::ShaderIOConfig{immediate_data_layout, options.bgra_swizzle_locations}',
     'raise::ShaderIOConfig{immediate_data_layout, options.bgra_swizzle_locations, options.depth_zero_to_one, options.disable_position_y_negation}')
edit('ref/dawn/src/tint/lang/glsl/writer/raise/shader_io.cc',
     'auto* mul = builder.Multiply(2_f, z);\n            auto* new_z = builder.Subtract(mul, w);',
     'core::ir::Value* new_z = z;\n            if (!config.depth_zero_to_one) {\n                auto* mul = builder.Multiply(2_f, z);\n                new_z = builder.Subtract(mul, w);\n            }')
edit('ref/dawn/src/utils/platform.h',
     '#if defined(_WIN32) || defined(_WIN64)',
     '#if defined(__SWITCH__)\n#define DAWN_PLATFORM_IS_SWITCH 1\n#define DAWN_PLATFORM_IS_POSIX 1\n#elif defined(_WIN32) || defined(_WIN64)')
