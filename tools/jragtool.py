#!/usr/bin/env python3
"""Inspect, generate and validate runtime Jolt ragdoll (.jrag) assets.

This front-end reuses smd2jrag.py for Valve SMD/QC/QCI parsing and hull
construction. It deliberately uses only the Python standard library.
"""

from __future__ import annotations

import argparse
import math
import shlex
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import smd2jrag as converter


MAX_JRAG_BYTES = 1024 * 1024
MAX_BODIES = 64
MAX_HULL_POINTS = 32
GEOMETRY_TOLERANCE = 0.000011


@dataclass
class RuntimeBody:
    name: str
    parent_index: int
    center: Tuple[float, float, float]
    joint_min: Tuple[float, float, float]
    joint_max: Tuple[float, float, float]
    angular_damping_override: float
    points: List[Tuple[float, float, float]]


@dataclass
class RuntimeDefinition:
    mass: float
    inertia_multiplier: float
    linear_damping: float
    angular_damping: float
    friction: float
    restitution: float
    bodies: List[RuntimeBody]


def _next_record(lines: Sequence[str], cursor: int, source: str):
    while cursor < len(lines):
        line_number = cursor + 1
        raw = lines[cursor]
        cursor += 1
        record = raw.split("#", 1)[0].strip()
        if not record:
            continue
        try:
            tokens = shlex.split(record, posix=True, comments=False)
        except ValueError as exc:
            raise ValueError("{}:{}: {}".format(source, line_number, exc)) from exc
        return tokens, line_number, cursor
    return None, 0, cursor


def _record(lines: Sequence[str], cursor: int, source: str,
            directive: str, value_count: int):
    tokens, line_number, cursor = _next_record(lines, cursor, source)
    if tokens is None:
        raise ValueError("{}: expected '{}' before end of file".format(source, directive))
    if len(tokens) != value_count + 1 or tokens[0] != directive:
        raise ValueError("{}:{}: expected '{}' with {} value(s)".format(
            source, line_number, directive, value_count))
    return tokens[1:], cursor


def _read_number(value: str, source: str, line_label: str) -> float:
    try:
        result = float(value)
    except ValueError as exc:
        raise ValueError("{}:{}: expected a number, got {!r}".format(
            source, line_label, value)) from exc
    if not math.isfinite(result):
        raise ValueError("{}:{}: values must be finite".format(source, line_label))
    return result


def _read_vector(lines: Sequence[str], cursor: int, source: str, directive: str):
    values, cursor = _record(lines, cursor, source, directive, 3)
    vector = tuple(_read_number(value, source, directive) for value in values)
    return vector, cursor


def parse_jrag_text(contents: str, source: str = "<memory>") -> RuntimeDefinition:
    if not contents or len(contents.encode("utf-8")) > MAX_JRAG_BYTES:
        raise ValueError("{}: file is empty or exceeds the 1 MiB runtime limit".format(source))

    lines = contents.replace("\r", "").splitlines()
    cursor = 0
    header, line_number, cursor = _next_record(lines, cursor, source)
    if header != ["JOLT_RAGDOLL", "1"]:
        raise ValueError("{}:{}: expected JOLT_RAGDOLL 1 header".format(source, line_number or 1))

    globals_out = []
    for directive in ("mass", "inertia_multiplier", "linear_damping",
                      "angular_damping", "friction", "restitution"):
        values, cursor = _record(lines, cursor, source, directive, 1)
        globals_out.append(_read_number(values[0], source, directive))

    mass, inertia, linear_damping, angular_damping, friction, restitution = globals_out
    if mass <= 0 or inertia <= 0:
        raise ValueError("{}: mass and inertia_multiplier must be positive".format(source))
    if linear_damping < 0 or angular_damping < 0 or friction < 0:
        raise ValueError("{}: damping and friction values cannot be negative".format(source))
    if restitution < 0 or restitution > 1:
        raise ValueError("{}: restitution must be between 0 and 1".format(source))

    values, cursor = _record(lines, cursor, source, "body_count", 1)
    try:
        body_count = int(values[0], 10)
    except ValueError as exc:
        raise ValueError("{}: body_count must be an integer".format(source)) from exc
    if body_count < 1 or body_count > MAX_BODIES:
        raise ValueError("{}: body_count must be between 1 and {}".format(source, MAX_BODIES))

    bodies: List[RuntimeBody] = []
    names = set()
    root_count = 0
    for body_index in range(body_count):
        body_tokens, body_line, cursor = _next_record(lines, cursor, source)
        if body_tokens is None or len(body_tokens) != 3 or body_tokens[0] != "body":
            raise ValueError("{}:{}: expected body \"name\" parent_index".format(
                source, body_line or "EOF"))
        name = body_tokens[1]
        try:
            parent_index = int(body_tokens[2], 10)
        except ValueError as exc:
            raise ValueError("{}:{}: parent index must be an integer".format(
                source, body_line)) from exc
        if not name or len(name.encode("utf-8")) > 64:
            raise ValueError("{}:{}: bone name must contain 1–64 UTF-8 bytes".format(
                source, body_line))
        key = name.casefold()
        if key in names:
            raise ValueError("{}:{}: duplicate physical bone {!r}".format(
                source, body_line, name))
        names.add(key)
        if parent_index == -1:
            root_count += 1
        elif parent_index < 0 or parent_index >= body_index:
            raise ValueError("{}:{}: parent must be -1 or an earlier body index".format(
                source, body_line))

        center, cursor = _read_vector(lines, cursor, source, "center")
        joint_min, cursor = _read_vector(lines, cursor, source, "joint_min")
        joint_max, cursor = _read_vector(lines, cursor, source, "joint_max")
        if any(low > high for low, high in zip(joint_min, joint_max)):
            raise ValueError("{}:{}: joint minimum exceeds maximum".format(
                source, body_line))
        values, cursor = _record(lines, cursor, source, "angular_damping_override", 1)
        damping_override = _read_number(values[0], source, "angular_damping_override")
        if damping_override < 0:
            raise ValueError("{}:{}: angular damping override cannot be negative".format(
                source, body_line))

        values, cursor = _record(lines, cursor, source, "hull", 1)
        try:
            point_count = int(values[0], 10)
        except ValueError as exc:
            raise ValueError("{}:{}: hull point count must be an integer".format(
                source, body_line)) from exc
        if point_count < 4 or point_count > MAX_HULL_POINTS:
            raise ValueError("{}:{}: hull point count must be between 4 and {}".format(
                source, body_line, MAX_HULL_POINTS))

        points = []
        for point_index in range(point_count):
            point, cursor = _read_vector(lines, cursor, source, "point")
            points.append(point)

        values, cursor = _record(lines, cursor, source, "end_body", 0)
        bodies.append(RuntimeBody(
            name=name,
            parent_index=parent_index,
            center=center,
            joint_min=joint_min,
            joint_max=joint_max,
            angular_damping_override=damping_override,
            points=points,
        ))

    if root_count != 1:
        raise ValueError("{}: the physical hierarchy must have exactly one root body".format(source))
    _values, cursor = _record(lines, cursor, source, "end", 0)
    trailing, line_number, cursor = _next_record(lines, cursor, source)
    if trailing is not None:
        raise ValueError("{}:{}: unexpected data after end".format(source, line_number))

    return RuntimeDefinition(
        mass=mass,
        inertia_multiplier=inertia,
        linear_damping=linear_damping,
        angular_damping=angular_damping,
        friction=friction,
        restitution=restitution,
        bodies=bodies,
    )


def read_jrag(path: Path) -> RuntimeDefinition:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ValueError("could not read .jrag '{}': {}".format(path, exc)) from exc
    if not data or len(data) > MAX_JRAG_BYTES:
        raise ValueError("{}: file is empty or exceeds the 1 MiB runtime limit".format(path))
    try:
        contents = data.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ValueError("{}: runtime .jrag must be UTF-8 text".format(path)) from exc
    return parse_jrag_text(contents, str(path))


def _close(a: float, b: float, tolerance: float = GEOMETRY_TOLERANCE) -> bool:
    return abs(a - b) <= tolerance


def _synthetic_qci(definition: RuntimeDefinition, source_path: Path):
    root = next(body for body in definition.bodies if body.parent_index == -1)
    return converter.QCIData(
        collision_name="",
        rootbone=root.name,
        mass=definition.mass,
        inertia=definition.inertia_multiplier,
        linear_damping=definition.linear_damping,
        angular_damping=definition.angular_damping,
        source_path=source_path,
        limits={
            body.name.casefold(): (list(body.joint_min), list(body.joint_max))
            for body in definition.bodies
        },
        angular_damping_overrides={
            body.name.casefold(): body.angular_damping_override
            for body in definition.bodies if body.angular_damping_override > 0
        },
    )


def validate_definition(jrag_path: Path, smd_path: Path,
                        qci_path: Optional[Path] = None) -> Tuple[RuntimeDefinition, int]:
    definition = read_jrag(jrag_path)
    smd = converter.parse_smd(smd_path)

    body_by_name = {body.name.casefold(): index for index, body in enumerate(definition.bodies)}
    node_by_name = {node.name.casefold(): index for index, node in smd.nodes.items()}
    missing = sorted(set(body_by_name) - set(node_by_name))
    if missing:
        raise ValueError("bones missing from SMD skeleton: {}".format(", ".join(missing)))

    collision_names = {smd.nodes[index].name.casefold() for index in smd.vertices}
    runtime_names = set(body_by_name)
    if collision_names != runtime_names:
        missing_runtime = sorted(collision_names - runtime_names)
        missing_mesh = sorted(runtime_names - collision_names)
        details = []
        if missing_runtime:
            details.append("SMD collision bones absent from .jrag: " + ", ".join(missing_runtime))
        if missing_mesh:
            details.append(".jrag bones without SMD collision geometry: " + ", ".join(missing_mesh))
        raise ValueError("; ".join(details))

    # Verify parent links against the nearest ancestor that also has a
    # collision body, exactly as the runtime and converter expect.
    body_index_by_node = {
        node_by_name[name]: index for name, index in body_by_name.items()
    }
    for index, body in enumerate(definition.bodies):
        node_index = node_by_name[body.name.casefold()]
        ancestor = smd.nodes[node_index].parent
        while ancestor != -1 and ancestor not in body_index_by_node:
            ancestor = smd.nodes[ancestor].parent
        expected_parent = body_index_by_node[ancestor] if ancestor != -1 else -1
        if body.parent_index != expected_parent:
            actual_name = "root" if body.parent_index < 0 else definition.bodies[body.parent_index].name
            expected_name = "root" if expected_parent < 0 else definition.bodies[expected_parent].name
            raise ValueError(
                "body {!r} has parent {!r} in .jrag; SMD hierarchy requires {!r}".format(
                    body.name, actual_name, expected_name))

    qci = converter.parse_qci(qci_path) if qci_path else _synthetic_qci(definition, smd_path)
    expected = converter.build_bodies(smd, qci)
    if len(expected) != len(definition.bodies):
        raise ValueError("SMD/QCI produces {} physical bodies, but .jrag contains {}".format(
            len(expected), len(definition.bodies)))

    if qci_path:
        for label, actual, source_value in (
            ("mass", definition.mass, qci.mass),
            ("inertia_multiplier", definition.inertia_multiplier, qci.inertia),
            ("linear_damping", definition.linear_damping, qci.linear_damping),
            ("angular_damping", definition.angular_damping, qci.angular_damping),
        ):
            if not _close(actual, source_value, 1.0e-6):
                raise ValueError(".jrag {} ({}) does not match QCI value ({})".format(
                    label, actual, source_value))

    for index, (actual, expected_body) in enumerate(zip(definition.bodies, expected)):
        if actual.name.casefold() != expected_body.name.casefold():
            raise ValueError("body {} is {!r}; SMD/QCI order requires {!r}".format(
                index, actual.name, expected_body.name))
        if actual.parent_index != expected_body.parent_index:
            raise ValueError("body {!r}: parent index does not match SMD/QCI hierarchy".format(
                actual.name))
        for label, actual_vector, expected_vector in (
            ("center", actual.center, expected_body.center),
            ("joint_min", actual.joint_min, expected_body.joint_min),
            ("joint_max", actual.joint_max, expected_body.joint_max),
        ):
            if any(not _close(a, b, 1.0e-6) for a, b in zip(actual_vector, expected_vector)):
                raise ValueError("body {!r}: {} does not match source data".format(actual.name, label))
        if not _close(actual.angular_damping_override,
                      expected_body.angular_damping_override, 1.0e-6):
            raise ValueError("body {!r}: angular damping override does not match source data".format(
                actual.name))
        if len(actual.points) != len(expected_body.points):
            raise ValueError("body {!r}: .jrag has {} hull points, source SMD produces {}".format(
                actual.name, len(actual.points), len(expected_body.points)))
        for point_index, (actual_point, expected_point) in enumerate(
                zip(actual.points, expected_body.points)):
            if any(not _close(a, b) for a, b in zip(actual_point, expected_point)):
                raise ValueError("body {!r}: hull point {} does not match source SMD".format(
                    actual.name, point_index))

    return definition, len(smd.nodes)


def inspect_smd(smd_path: Path, qci_path: Optional[Path] = None) -> None:
    smd = converter.parse_smd(smd_path)
    transforms = converter.world_transforms(smd)
    del transforms  # Parsing the complete reference hierarchy also checks cycles.
    collision_ids = set(smd.vertices)
    physical_parent: Dict[int, Optional[int]] = {}
    for index in collision_ids:
        parent = smd.nodes[index].parent
        while parent != -1 and parent not in collision_ids:
            parent = smd.nodes[parent].parent
        physical_parent[index] = parent if parent in collision_ids else None

    print("SMD: {}".format(smd_path))
    print("Skeleton bones: {}".format(len(smd.nodes)))
    print("Collision bodies: {}".format(len(collision_ids)))
    print("")
    for index in sorted(collision_ids):
        node = smd.nodes[index]
        unique = converter._deduplicate(smd.vertices[index])
        status = "valid"
        try:
            converter._check_3d_hull(unique, node.name)
        except ValueError as exc:
            status = "INVALID: {}".format(exc)
        parent_id = physical_parent[index]
        parent_name = smd.nodes[parent_id].name if parent_id is not None else "root"
        print('  {:<24} parent={:<24} vertices={:3d} unique={:2d} hull={}'.format(
            node.name, parent_name, len(smd.vertices[index]), len(unique), status))

    if qci_path:
        qci = converter.parse_qci(qci_path)
        for warning in converter.unbound_qci_directives(smd, qci):
            print("WARNING: {}".format(warning))
        print("")
        print("QCI: {}".format(qci_path))
        print("Collision block: {}".format(qci.collision_name))
        print("Root bone: {}".format(qci.rootbone))
        print("Mass: {}  inertia: {}  damping: {} / {}".format(
            qci.mass, qci.inertia, qci.linear_damping, qci.angular_damping))
        bodies = converter.build_bodies(smd, qci)
        print("Generated physical hierarchy: {} bodies".format(len(bodies)))
        for index, body in enumerate(bodies):
            parent = "root" if body.parent_index < 0 else bodies[body.parent_index].name
            print("  {:2d}. {:<24} parent={} hull_points={}".format(
                index, body.name, parent, len(body.points)))


def _resolve_smd(source_path: Path, explicit_smd: Optional[Path]) -> Path:
    if explicit_smd is not None:
        return explicit_smd
    qci = converter.parse_qci(source_path)
    collision_file = qci.collision_name
    if Path(collision_file).suffix.lower() != ".smd":
        collision_file += ".smd"
    return qci.source_path.parent / collision_file


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="jragtool",
        description="Inspect, generate and validate Xash3D/Jolt .jrag ragdoll assets.")
    parser.add_argument("--version", action="version", version="jragtool 1.0")
    commands = parser.add_subparsers(dest="command", required=True)

    inspect_parser = commands.add_parser("inspect", help="inspect an SMD skeleton and collision meshes")
    inspect_parser.add_argument("smd", type=Path, help="Valve collision SMD")
    inspect_parser.add_argument("--qci", type=Path,
                                help="optional QC/QCI source for joint limits and mass settings")

    generate_parser = commands.add_parser("generate", help="generate a runtime .jrag from QC/QCI and SMD")
    generate_parser.add_argument("source", type=Path, help="model QC or ragdoll QCI")
    generate_parser.add_argument("--smd", type=Path, help="override the collision SMD path")
    generate_parser.add_argument("-o", "--output", type=Path, required=True,
                                 help="output runtime .jrag path")
    generate_parser.add_argument("--friction", type=float, default=0.8,
                                 help="Jolt friction (default: 0.8)")
    generate_parser.add_argument("--restitution", type=float, default=0.0,
                                 help="Jolt restitution from 0 to 1 (default: 0)")

    validate_parser = commands.add_parser("validate", help="validate a .jrag against its source SMD")
    validate_parser.add_argument("jrag", type=Path, help="runtime .jrag file")
    validate_parser.add_argument("smd", type=Path, help="source collision SMD")
    validate_parser.add_argument("--qci", type=Path,
                                 help="optional QC/QCI source; also checks authored physics settings")

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "inspect":
            inspect_smd(args.smd, args.qci)
            return 0
        if args.command == "generate":
            source_qci = converter.parse_qci(args.source)
            smd_path = _resolve_smd(args.source, args.smd)
            source_smd = converter.parse_smd(smd_path)
            for warning in converter.unbound_qci_directives(source_smd, source_qci):
                print("WARNING: {}".format(warning))
            bodies = converter.convert(
                args.source, args.smd, args.output,
                friction=args.friction, restitution=args.restitution)
            definition, _bone_count = validate_definition(
                args.output, smd_path, args.source)
            print("Generated and validated {} ({} rigid bodies).".format(
                args.output, len(definition.bodies)))
            for index, body in enumerate(bodies):
                parent = "root" if body.parent_index < 0 else "body {}".format(body.parent_index)
                print("  {:2d}. {:<24} parent={:<8} hull_points={:2d}".format(
                    index, body.name, parent, len(body.points)))
            return 0
        if args.command == "validate":
            definition, bone_count = validate_definition(args.jrag, args.smd, args.qci)
            if args.qci:
                source_qci = converter.parse_qci(args.qci)
                source_smd = converter.parse_smd(args.smd)
                for warning in converter.unbound_qci_directives(source_smd, source_qci):
                    print("WARNING: {}".format(warning))
            print("Valid: {} ({} bodies; {} SMD skeleton bones).".format(
                args.jrag, len(definition.bodies), bone_count))
            print("  Geometry, hull points, body order and physical parent links match.")
            if args.qci:
                print("  Global physics settings and joint limits match the QC/QCI source.")
            return 0
        parser.error("unknown command")
    except (OSError, ValueError) as exc:
        print("jragtool: error: {}".format(exc), file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    sys.exit(main())
