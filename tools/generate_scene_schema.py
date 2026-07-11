#!/usr/bin/env python3
import pathlib
import re
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: generate_scene_schema.py INPUT OUTPUT")
source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
component_pattern = re.compile(
    r"^VE_SCENE_COMPONENT\(([^,\r\n]+),\s*([^,\r\n]+),\s*([^)\r\n]+)\)\s*$",
    re.MULTILINE)
property_pattern = re.compile(
    r"^VE_SCENE_PROPERTY\(([^,\r\n]+),\s*([^,\r\n]+),\s*([^,\r\n]+),\s*([^,\r\n]+),\s*([^,\r\n]+),\s*([^,\r\n]+),\s*([^,\r\n]+),\s*([^)\r\n]+)\)\s*$",
    re.MULTILINE)
components = []
for match in component_pattern.finditer(source):
    components.append({"symbol": match.group(1).strip(), "name": match.group(2).strip(),
                       "version": int(match.group(3).strip()), "properties": []})
by_symbol = {component["symbol"]: component for component in components}
for match in property_pattern.finditer(source):
    symbol = match.group(1).strip()
    if symbol not in by_symbol:
        raise SystemExit(f"property references unknown component {symbol}")
    by_symbol[symbol]["properties"].append(tuple(value.strip() for value in match.groups()[1:]))
if not components:
    raise SystemExit("annotated scene schema contains no components")
lines = ["#pragma once", "", "#include \"scene/SceneReflection.hpp\"", "", "namespace ve {", "",
         "inline SceneTypeRegistry generatedSceneTypeRegistry() {", "  SceneTypeRegistry registry;"]
for component in components:
    lines.append("  registry.registerType(makeSceneTypeMetadata(")
    lines.append(f"      \"{component['symbol']}\", \"{component['name']}\", {component['version']}U, {{")
    for display, name, kind, minimum, maximum, step, binding in component["properties"]:
        lines.append("          makeScenePropertyMetadata(")
        lines.append(f"              \"{display}\", \"{name}\", ScenePropertyKind::{kind},")
        lines.append(f"              {minimum}, {maximum}, {step}, \"{binding}\"),")
    lines.append("      }));")
lines.extend(["  bindBuiltinSceneTypeHooks(registry);", "  return registry;", "}", "", "} // namespace ve", ""])
output = pathlib.Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
content = "\n".join(lines)
if not output.exists() or output.read_text(encoding="utf-8") != content:
    output.write_text(content, encoding="utf-8")
