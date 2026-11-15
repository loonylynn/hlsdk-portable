#!/usr/bin/env python3
import math
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS_DIR))

import smd2jrag


class SMDToJRAGTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.qci = self.root / "ragdoll.qci"
        self.smd = self.root / "ragdoll.smd"
        self.output = self.root / "test.jrag"

        # The collision bone is beneath two non-physical skeleton bones and
        # has a rotated parent. Vertices are authored in model space.
        self.qci.write_text(
            '$collisionjoints ragdoll {\n'
            '    $mass 115\n'
            '    $inertia 10\n'
            '    $damping 0.01\n'
            '    $rotdamping 1.5\n'
            '    $rootbone "Bip01"\n'
            '    $jointconstrain "Bip01 L Leg" x limit -25 20\n'
            '    $jointrotdamping "Bip01 L Leg" 5\n'
            '}\n',
            encoding="utf-8",
        )

        nodes = (
            'version 1\n'
            'nodes\n'
            '0 "Bip01" -1\n'
            '1 "Bip01 Helper" 0\n'
            '2 "Bip01 L Leg" 1\n'
            'end\n'
            'skeleton\n'
            'time 1\n'
            '0 0 0 0 0 0 0\n'
            '1 2 0 0 0 0 1.5707963267948966\n'
            '2 3 0 0 0 0 0\n'
            'triangles\n'
        )
        # Bone 2 has world position (2, 3, 0) and a 90-degree Z rotation.
        # Generate a local cube, then express its vertices in model space.
        local = [
            (-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
            (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1),
        ]
        world = [(-p[1] + 2, p[0] + 3, p[2]) for p in local]
        faces = [
            (0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7),
            (0, 1, 5), (0, 5, 4), (3, 7, 6), (3, 6, 2),
            (0, 4, 7), (0, 7, 3), (1, 2, 6), (1, 6, 5),
        ]
        triangles = []
        for face in faces:
            triangles.append("collision_material")
            for vertex_index in face:
                x, y, z = world[vertex_index]
                triangles.append(
                    "2 {} {} {} 0 0 1 0 0".format(x, y, z)
                )
        self.smd.write_text(nodes + "\n".join(triangles) + "\n", encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def test_convert_preserves_local_hull_and_qci_settings(self):
        bodies = smd2jrag.convert(self.qci, self.smd, self.output)
        self.assertEqual(len(bodies), 1)
        body = bodies[0]
        self.assertEqual(body.name, "Bip01 L Leg")
        self.assertEqual(body.parent_index, -1)
        self.assertEqual(len(body.points), 8)
        self.assertEqual(body.center, (0.0, 0.0, 0.0))
        self.assertEqual(body.joint_min, (-25.0, -1.0e30, -1.0e30))
        self.assertEqual(body.joint_max, (20.0, 1.0e30, 1.0e30))
        self.assertEqual(body.angular_damping_override, 5.0)

        output = self.output.read_text(encoding="utf-8")
        self.assertIn("JOLT_RAGDOLL 1", output)
        self.assertIn("mass 115", output)
        self.assertIn("inertia_multiplier 10", output)
        self.assertIn("body_count 1", output)
        self.assertIn('body "Bip01 L Leg" -1', output)
        self.assertIn("friction 0.8", output)
        self.assertIn("restitution 0", output)

        actual = {tuple(round(v, 5) for v in point) for point in body.points}
        expected = {
            (-1.0, -1.0, -1.0), (1.0, -1.0, -1.0),
            (1.0, 1.0, -1.0), (-1.0, 1.0, -1.0),
            (-1.0, -1.0, 1.0), (1.0, -1.0, 1.0),
            (1.0, 1.0, 1.0), (-1.0, 1.0, 1.0),
        }
        self.assertEqual(actual, expected)

    def test_rejects_invalid_restitution(self):
        qci = smd2jrag.parse_qci(self.qci)
        bodies = smd2jrag.build_bodies(smd2jrag.parse_smd(self.smd), qci)
        with self.assertRaisesRegex(ValueError, "restitution"):
            smd2jrag.render_jrag(qci, bodies, restitution=1.1)

    def test_parent_mapping_uses_nearest_physical_ancestor(self):
        smd = smd2jrag.parse_smd(self.smd)
        qci = smd2jrag.parse_qci(self.qci)

        # Give the helper bone its own cube. Bone 2 is a child of bone 1 in
        # the studio skeleton, so it must attach to physical body 0 rather
        # than to the non-physical $rootbone or a studio-only intermediary.
        local = [
            (-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
            (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1),
        ]
        smd.vertices[1] = [(-p[1] + 2, p[0], p[2]) for p in local]

        bodies = smd2jrag.build_bodies(smd, qci)
        self.assertEqual(len(bodies), 2)
        self.assertEqual(bodies[0].name, "Bip01 Helper")
        self.assertEqual(bodies[0].parent_index, -1)
        self.assertEqual(bodies[1].name, "Bip01 L Leg")
        self.assertEqual(bodies[1].parent_index, 0)


if __name__ == "__main__":
    unittest.main()
