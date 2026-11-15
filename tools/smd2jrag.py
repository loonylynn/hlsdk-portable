#!/usr/bin/env python3
"""Convert a Valve SMD collision mesh plus QC/QCI ragdoll directives to Jolt .jrag.

The SMD triangle vertices are model-space positions assigned to bones. This tool
uses the SMD reference skeleton to transform each bone's vertices into that
bone's local space, then emits a per-bone point cloud for Jolt's convex hull
builder. No third-party Python packages are required.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shlex
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple


Vec3 = Tuple[float, float, float]
Mat3 = List[List[float]]
UNLIMITED = 1.0e30


@dataclass
class BoneNode:
    index: int
    name: str
    parent: int


@dataclass
class SMDData:
    nodes: Dict[int, BoneNode]
    pose: Dict[int, Tuple[float, float, float, float, float, float]]
    vertices: Dict[int, List[Vec3]]


@dataclass
class QCIData:
    collision_name: str
    rootbone: str
    mass: float
    inertia: float
    linear_damping: float
    angular_damping: float
    limits: Dict[str, Tuple[List[float], List[float]]] = field(default_factory=dict)
    angular_damping_overrides: Dict[str, float] = field(default_factory=dict)


@dataclass
class OutputBody:
    name: str
    parent_index: int
    center: Vec3
    points: List[Vec3]
    joint_min: Vec3
    joint_max: Vec3
    angular_damping_override: float


def _finite_float(value: str, context: str) -> float:
    try:
        number = float(value)
    except ValueError as exc:
        raise ValueError("{}: expected a number, got {!r}".format(context, value)) from exc
    if not math.isfinite(number):
        raise ValueError("{}: value must be finite".format(context))
    return number


def _tokens(line: str, context: str) -> List[str]:
    try:
        return shlex.split(line, posix=True, comments=False)
    except ValueError as exc:
        raise ValueError("{}: {}".format(context, exc)) from exc


def parse_smd(path: Path) -> SMDData:
    try:
        lines = path.read_text(encoding="utf-8-sig").replace("\r", "").splitlines()
    except OSError as exc:
        raise ValueError("could not read SMD '{}': {}".format(path, exc)) from exc

    def section_index(name: str) -> int:
        for i, line in enumerate(lines):
            if line.strip().lower() == name:
                return i
        raise ValueError("{}: missing '{}' section".format(path, name))

    nodes_start = section_index("nodes")
    nodes_end = next((i for i in range(nodes_start + 1, len(lines))
                      if lines[i].strip().lower() == "end"), None)
    if nodes_end is None:
        raise ValueError("{}: unterminated nodes section".format(path))

    nodes: Dict[int, BoneNode] = {}
    folded_names: Dict[str, int] = {}
    for line_number, line in enumerate(lines[nodes_start + 1:nodes_end], nodes_start + 2):
        if not line.strip():
            continue
        parts = _tokens(line, "{}:{}".format(path, line_number))
        if len(parts) != 3:
            raise ValueError("{}:{}: expected '<index> \"<bone name>\" <parent>'".format(path, line_number))
        try:
            index = int(parts[0], 10)
            parent = int(parts[2], 10)
        except ValueError as exc:
            raise ValueError("{}:{}: invalid bone index/parent".format(path, line_number)) from exc
        name = parts[1]
        if index in nodes:
            raise ValueError("{}:{}: duplicate bone index {}".format(path, line_number, index))
        folded = name.casefold()
        if folded in folded_names:
            raise ValueError("{}:{}: duplicate bone name {!r}".format(path, line_number, name))
        nodes[index] = BoneNode(index, name, parent)
        folded_names[folded] = index

    if not nodes:
        raise ValueError("{}: nodes section is empty".format(path))
    for node in nodes.values():
        if node.parent != -1 and node.parent not in nodes:
            raise ValueError("{}: bone {!r} refers to missing parent index {}".format(
                path, node.name, node.parent))

    skeleton_start = section_index("skeleton")
    triangles_start = section_index("triangles")
    if triangles_start <= skeleton_start:
        raise ValueError("{}: triangles section appears before skeleton".format(path))

    frames: Dict[int, Dict[int, Tuple[float, float, float, float, float, float]]] = {}
    frame_index: Optional[int] = None
    for line_number, line in enumerate(lines[skeleton_start + 1:triangles_start], skeleton_start + 2):
        stripped = line.strip()
        if not stripped:
            continue
        parts = stripped.split()
        if len(parts) == 2 and parts[0].lower() == "time":
            try:
                frame_index = int(parts[1], 10)
            except ValueError as exc:
                raise ValueError("{}:{}: invalid skeleton frame".format(path, line_number)) from exc
            frames.setdefault(frame_index, {})
            continue
        if frame_index is None:
            continue
        if len(parts) != 7:
            raise ValueError("{}:{}: expected bone index and six transform values".format(path, line_number))
        try:
            bone_index = int(parts[0], 10)
        except ValueError as exc:
            raise ValueError("{}:{}: invalid skeleton bone index".format(path, line_number)) from exc
        if bone_index not in nodes:
            raise ValueError("{}:{}: skeleton references unknown bone {}".format(path, line_number, bone_index))
        values = tuple(_finite_float(value, "{}:{}".format(path, line_number)) for value in parts[1:])
        if bone_index in frames[frame_index]:
            raise ValueError("{}:{}: duplicate transform for bone {}".format(path, line_number, bone_index))
        frames[frame_index][bone_index] = values  # type: ignore[assignment]

    if not frames:
        raise ValueError("{}: skeleton contains no time frame".format(path))
    # Prefer the conventional reference frame 0. Some compiled ragdoll SMDs
    # contain only time 1; in that case use their first (lowest) frame.
    selected_frame = 0 if 0 in frames else min(frames)
    pose = frames[selected_frame]
    missing_pose = sorted(set(nodes) - set(pose))
    if missing_pose:
        raise ValueError("{}: frame {} has no transforms for bone indices {}".format(
            path, selected_frame, ", ".join(map(str, missing_pose[:12]))))

    vertices: Dict[int, List[Vec3]] = {}
    for line_number, line in enumerate(lines[triangles_start + 1:], triangles_start + 2):
        parts = line.split()
        # A material name precedes each triangle. Vertex records start with
        # an integer bone index and then contain XYZ, normal and UV values.
        if len(parts) < 9:
            continue
        try:
            bone_index = int(parts[0], 10)
            xyz = tuple(_finite_float(value, "{}:{}".format(path, line_number)) for value in parts[1:4])
        except ValueError:
            continue
        if bone_index not in nodes:
            raise ValueError("{}:{}: triangle vertex references unknown bone {}".format(
                path, line_number, bone_index))
        vertices.setdefault(bone_index, []).append(xyz)  # type: ignore[arg-type]

    if not vertices:
        raise ValueError("{}: triangles section contains no valid SMD vertices".format(path))
    return SMDData(nodes=nodes, pose=pose, vertices=vertices)


def _strip_qci_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def parse_qci(path: Path) -> QCIData:
    try:
        source = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        raise ValueError("could not read QCI '{}': {}".format(path, exc)) from exc
    source = _strip_qci_comments(source)
    match = re.search(
        r'^\s*\$collisionjoints\s+(?:"([^"]+)"|([^\s{]+))\s*\{(.*?)^\s*\}',
        source, flags=re.IGNORECASE | re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError("{}: could not find a '$collisionjoints name { ... }' block".format(path))
    collision_name = match.group(1) or match.group(2)

    mass = inertia = linear_damping = angular_damping = None
    rootbone: Optional[str] = None
    limits: Dict[str, Tuple[List[float], List[float]]] = {}
    damping_overrides: Dict[str, float] = {}

    for line_number, line in enumerate(match.group(3).splitlines(), 1):
        if not line.strip():
            continue
        parts = _tokens(line, "{}:{}".format(path, line_number))
        if not parts:
            continue
        directive = parts[0].lower()
        if directive in ("$mass", "$inertia", "$damping", "$rotdamping"):
            if len(parts) != 2:
                raise ValueError("{}:{}: {} expects one value".format(path, line_number, directive))
            value = _finite_float(parts[1], "{}:{}".format(path, line_number))
            if directive == "$mass":
                mass = value
            elif directive == "$inertia":
                inertia = value
            elif directive == "$damping":
                linear_damping = value
            else:
                angular_damping = value
        elif directive == "$rootbone":
            if len(parts) != 2:
                raise ValueError("{}:{}: $rootbone expects one bone name".format(path, line_number))
            rootbone = parts[1]
        elif directive == "$jointconstrain":
            if len(parts) != 6 or parts[3].lower() != "limit":
                raise ValueError("{}:{}: expected '$jointconstrain \"bone\" <x|y|z> limit <min> <max>'".format(
                    path, line_number))
            bone_key = parts[1].casefold()
            axis = parts[2].lower()
            if axis not in ("x", "y", "z"):
                raise ValueError("{}:{}: unsupported joint axis {!r}".format(path, line_number, axis))
            low = _finite_float(parts[4], "{}:{}".format(path, line_number))
            high = _finite_float(parts[5], "{}:{}".format(path, line_number))
            if low > high:
                raise ValueError("{}:{}: joint minimum exceeds maximum".format(path, line_number))
            if bone_key not in limits:
                limits[bone_key] = ([-UNLIMITED] * 3, [UNLIMITED] * 3)
            axis_index = ("x", "y", "z").index(axis)
            limits[bone_key][0][axis_index] = low
            limits[bone_key][1][axis_index] = high
        elif directive == "$jointrotdamping":
            if len(parts) != 3:
                raise ValueError("{}:{}: $jointrotdamping expects a bone name and value".format(
                    path, line_number))
            value = _finite_float(parts[2], "{}:{}".format(path, line_number))
            if value < 0:
                raise ValueError("{}:{}: joint rotational damping cannot be negative".format(path, line_number))
            damping_overrides[parts[1].casefold()] = value

    required = {
        "$mass": mass,
        "$inertia": inertia,
        "$damping": linear_damping,
        "$rotdamping": angular_damping,
        "$rootbone": rootbone,
    }
    missing = [name for name, value in required.items() if value is None]
    if missing:
        raise ValueError("{}: collisionjoints block is missing {}".format(path, ", ".join(missing)))
    if mass <= 0 or inertia <= 0 or linear_damping < 0 or angular_damping < 0:
        raise ValueError("{}: mass/inertia must be positive and damping values non-negative".format(path))

    return QCIData(
        collision_name=collision_name,
        rootbone=rootbone,  # type: ignore[arg-type]
        mass=mass,
        inertia=inertia,
        linear_damping=linear_damping,
        angular_damping=angular_damping,
        limits=limits,
        angular_damping_overrides=damping_overrides,
    )


def _mat_mul(a: Mat3, b: Mat3) -> Mat3:
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def _mat_vec(matrix: Mat3, vector: Sequence[float]) -> Vec3:
    return tuple(sum(matrix[i][j] * vector[j] for j in range(3)) for i in range(3))  # type: ignore[return-value]


def _transpose(matrix: Mat3) -> Mat3:
    return [[matrix[j][i] for j in range(3)] for i in range(3)]


def _rotation_xyz(rx: float, ry: float, rz: float) -> Mat3:
    """SMD Euler rotation: Rz * Ry * Rx."""
    sx, cx = math.sin(rx), math.cos(rx)
    sy, cy = math.sin(ry), math.cos(ry)
    sz, cz = math.sin(rz), math.cos(rz)
    rx_m = [[1.0, 0.0, 0.0], [0.0, cx, -sx], [0.0, sx, cx]]
    ry_m = [[cy, 0.0, sy], [0.0, 1.0, 0.0], [-sy, 0.0, cy]]
    rz_m = [[cz, -sz, 0.0], [sz, cz, 0.0], [0.0, 0.0, 1.0]]
    return _mat_mul(rz_m, _mat_mul(ry_m, rx_m))


def world_transforms(smd: SMDData) -> Dict[int, Tuple[Mat3, Vec3]]:
    cache: Dict[int, Tuple[Mat3, Vec3]] = {}
    visiting = set()

    def resolve(index: int) -> Tuple[Mat3, Vec3]:
        if index in cache:
            return cache[index]
        if index in visiting:
            raise ValueError("SMD bone hierarchy contains a cycle at {!r}".format(smd.nodes[index].name))
        visiting.add(index)
        node = smd.nodes[index]
        x, y, z, rx, ry, rz = smd.pose[index]
        local_rotation = _rotation_xyz(rx, ry, rz)
        local_translation: Vec3 = (x, y, z)
        if node.parent == -1:
            result = (local_rotation, local_translation)
        else:
            parent_rotation, parent_translation = resolve(node.parent)
            result_rotation = _mat_mul(parent_rotation, local_rotation)
            rotated_translation = _mat_vec(parent_rotation, local_translation)
            result_translation = tuple(parent_translation[i] + rotated_translation[i] for i in range(3))
            result = (result_rotation, result_translation)  # type: ignore[assignment]
        visiting.remove(index)
        cache[index] = result
        return result

    for index in smd.nodes:
        resolve(index)
    return cache


def _deduplicate(points: Sequence[Vec3]) -> List[Vec3]:
    result: List[Vec3] = []
    seen = set()
    for point in points:
        key = tuple(round(value, 6) for value in point)
        if key not in seen:
            seen.add(key)
            result.append(point)
    return result


def _check_3d_hull(points: Sequence[Vec3], bone_name: str) -> None:
    if len(points) < 4:
        raise ValueError("bone {!r}: collision mesh has only {} unique vertices; at least 4 are required".format(
            bone_name, len(points)))
    if len(points) > 32:
        raise ValueError(
            "bone {!r}: collision mesh has {} unique vertices (the .jrag runtime limit is 32). "
            "Simplify this bone's convex collision mesh to 32 vertices or fewer.".format(bone_name, len(points)))

    origin = points[0]
    scale = max(math.dist(origin, point) for point in points[1:])
    if scale <= 1.0e-9:
        raise ValueError("bone {!r}: collision mesh has zero size".format(bone_name))
    v1 = None
    for point in points[1:]:
        candidate = tuple(point[i] - origin[i] for i in range(3))
        if math.sqrt(sum(value * value for value in candidate)) > scale * 1.0e-8:
            v1 = candidate
            break
    if v1 is None:
        raise ValueError("bone {!r}: collision mesh is degenerate".format(bone_name))

    normal = None
    for point in points[1:]:
        v2 = tuple(point[i] - origin[i] for i in range(3))
        cross = (
            v1[1] * v2[2] - v1[2] * v2[1],
            v1[2] * v2[0] - v1[0] * v2[2],
            v1[0] * v2[1] - v1[1] * v2[0],
        )
        magnitude = math.sqrt(sum(value * value for value in cross))
        if magnitude > scale * scale * 1.0e-8:
            normal = cross
            break
    if normal is None:
        raise ValueError("bone {!r}: all collision vertices are collinear".format(bone_name))

    volume_tolerance = max(1.0e-12, scale * scale * scale * 1.0e-9)
    for point in points[1:]:
        delta = tuple(point[i] - origin[i] for i in range(3))
        volume6 = sum(normal[i] * delta[i] for i in range(3))
        if abs(volume6) > volume_tolerance:
            return
    raise ValueError("bone {!r}: collision vertices are coplanar; a 3D convex hull is required".format(bone_name))


def _find_bone(nodes: Dict[int, BoneNode], name: str) -> int:
    matches = [index for index, node in nodes.items() if node.name.casefold() == name.casefold()]
    if not matches:
        raise ValueError("bone {!r} from QCI was not found in the SMD nodes".format(name))
    if len(matches) != 1:
        raise ValueError("bone name {!r} is ambiguous in the SMD".format(name))
    return matches[0]


def build_bodies(smd: SMDData, qci: QCIData) -> List[OutputBody]:
    root_id = _find_bone(smd.nodes, qci.rootbone)
    transforms = world_transforms(smd)
    collider_ids = set(smd.vertices)
    if not collider_ids:
        raise ValueError("SMD has no collision geometry")

    # Every constrained body must have a corresponding collision mesh.
    body_names = {smd.nodes[index].name.casefold(): index for index in collider_ids}
    for name in qci.limits:
        if name not in body_names:
            raise ValueError("QCI constrains bone {!r}, but that bone has no SMD collision mesh".format(name))
    for name in qci.angular_damping_overrides:
        if name not in body_names:
            raise ValueError("QCI assigns rotational damping to {!r}, but that bone has no SMD collision mesh".format(
                name))

    parent_body: Dict[int, Optional[int]] = {}
    for index in collider_ids:
        ancestor = smd.nodes[index].parent
        while ancestor != -1 and ancestor not in collider_ids:
            ancestor = smd.nodes[ancestor].parent
        parent_body[index] = ancestor if ancestor in collider_ids else None

    root_bodies = [index for index, parent in parent_body.items() if parent is None]
    if len(root_bodies) != 1:
        names = ", ".join(smd.nodes[index].name for index in root_bodies)
        raise ValueError("expected exactly one root collision body, found {} ({})".format(
            len(root_bodies), names or "none"))

    # $rootbone may be a non-physical skeleton anchor above the root collision
    # body (as it is in the shipped zombie QCI).
    for index in collider_ids:
        ancestor = index
        while ancestor != -1 and ancestor != root_id:
            ancestor = smd.nodes[ancestor].parent
        if ancestor != root_id:
            raise ValueError("collision body {!r} is outside QCI rootbone {!r}".format(
                smd.nodes[index].name, qci.rootbone))

    # Topological order, with original SMD bone indices as deterministic tie-break.
    order: List[int] = []
    remaining = set(collider_ids)
    while remaining:
        ready = sorted(index for index in remaining
                       if parent_body[index] is None or parent_body[index] in order)
        if not ready:
            raise ValueError("collision body hierarchy contains a cycle")
        chosen = ready[0]
        order.append(chosen)
        remaining.remove(chosen)

    index_for_bone = {bone_index: body_index for body_index, bone_index in enumerate(order)}
    output: List[OutputBody] = []
    for bone_index in order:
        node = smd.nodes[bone_index]
        rotation, translation = transforms[bone_index]
        inverse_rotation = _transpose(rotation)
        local_points = []
        for vertex in smd.vertices[bone_index]:
            relative = tuple(vertex[i] - translation[i] for i in range(3))
            local_points.append(_mat_vec(inverse_rotation, relative))
        points = _deduplicate(local_points)
        _check_3d_hull(points, node.name)
        center = tuple(sum(point[i] for point in points) / len(points) for i in range(3))
        hull_points = [tuple(point[i] - center[i] for i in range(3)) for point in points]

        limits = qci.limits.get(node.name.casefold())
        joint_min = tuple(limits[0]) if limits else (-UNLIMITED, -UNLIMITED, -UNLIMITED)
        joint_max = tuple(limits[1]) if limits else (UNLIMITED, UNLIMITED, UNLIMITED)
        damping = qci.angular_damping_overrides.get(node.name.casefold(), 0.0)
        parent = parent_body[bone_index]
        parent_index = index_for_bone[parent] if parent is not None else -1
        output.append(OutputBody(
            name=node.name,
            parent_index=parent_index,
            center=center,  # type: ignore[arg-type]
            points=hull_points,
            joint_min=joint_min,  # type: ignore[arg-type]
            joint_max=joint_max,  # type: ignore[arg-type]
            angular_damping_override=damping,
        ))
    if len(output) > 64:
        raise ValueError("SMD contains {} collision bodies; the .jrag runtime limit is 64".format(len(output)))
    return output


def _format_fixed(value: float) -> str:
    if abs(value) < 0.0000005:
        value = 0.0
    return "{:.6f}".format(value)


def _format_scalar(value: float) -> str:
    if abs(value) < 1.0e-12:
        value = 0.0
    return "{:.9g}".format(value)


def render_jrag(qci: QCIData, bodies: Sequence[OutputBody],
                friction: float = 0.8, restitution: float = 0.0) -> str:
    if not math.isfinite(friction) or friction < 0:
        raise ValueError("friction must be a finite non-negative value")
    if not math.isfinite(restitution) or restitution < 0 or restitution > 1:
        raise ValueError("restitution must be between 0 and 1")
    if not bodies:
        raise ValueError("cannot write a ragdoll with no bodies")

    lines = [
        "# Jolt ragdoll definition v1",
        "# Generated by tools/smd2jrag.py; edit the source SMD/QCI instead.",
        "# GoldSrc units and degrees.",
        "JOLT_RAGDOLL 1",
        "mass {}".format(_format_scalar(qci.mass)),
        "inertia_multiplier {}".format(_format_scalar(qci.inertia)),
        "linear_damping {}".format(_format_scalar(qci.linear_damping)),
        "angular_damping {}".format(_format_scalar(qci.angular_damping)),
        "friction {}".format(_format_scalar(friction)),
        "restitution {}".format(_format_scalar(restitution)),
        "body_count {}".format(len(bodies)),
        "",
    ]
    for body in bodies:
        lines.append("body {} {}".format(json.dumps(body.name, ensure_ascii=False), body.parent_index))
        lines.append("center {}".format(" ".join(_format_fixed(value) for value in body.center)))
        lines.append("joint_min {}".format(" ".join(_format_scalar(value) for value in body.joint_min)))
        lines.append("joint_max {}".format(" ".join(_format_scalar(value) for value in body.joint_max)))
        lines.append("angular_damping_override {}".format(
            _format_scalar(body.angular_damping_override)))
        lines.append("hull {}".format(len(body.points)))
        for point in body.points:
            lines.append("point {}".format(" ".join(_format_fixed(value) for value in point)))
        lines.append("end_body")
        lines.append("")
    lines.append("end")
    return "\n".join(lines) + "\n"


def convert(qci_path: Path, smd_path: Optional[Path], output_path: Path,
            friction: float = 0.8, restitution: float = 0.0) -> List[OutputBody]:
    qci = parse_qci(qci_path)
    if smd_path is None:
        smd_path = qci_path.parent / (qci.collision_name + ".smd")
    smd = parse_smd(smd_path)
    bodies = build_bodies(smd, qci)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(render_jrag(qci, bodies, friction, restitution), encoding="utf-8")
    return bodies


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Convert a Valve ragdoll collision SMD and its QC/QCI directives to a Jolt .jrag file.")
    parser.add_argument("qci", type=Path, help="QC/QCI file containing a $collisionjoints block")
    parser.add_argument("--smd", type=Path,
                        help="collision SMD; defaults to <collisionjoints name>.smd beside the QCI")
    parser.add_argument("-o", "--output", type=Path,
                        help="output .jrag path (default: QCI path with .jrag extension)")
    parser.add_argument("--friction", type=float, default=0.8,
                        help="Jolt friction (default: 0.8; not encoded by Valve's QCI directives)")
    parser.add_argument("--restitution", type=float, default=0.0,
                        help="Jolt restitution, from 0 to 1 (default: 0)")
    args = parser.parse_args(argv)

    output_path = args.output or args.qci.with_suffix(".jrag")
    try:
        bodies = convert(args.qci, args.smd, output_path, args.friction, args.restitution)
    except (OSError, ValueError) as exc:
        print("smd2jrag: error: {}".format(exc), file=sys.stderr)
        return 2

    print("Wrote {} ({} rigid bodies)".format(output_path, len(bodies)))
    for body in bodies:
        parent = "root" if body.parent_index < 0 else "body {}".format(body.parent_index)
        print("  {:<20} parent={:<8} hull_points={:2d}".format(
            body.name, parent, len(body.points)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
