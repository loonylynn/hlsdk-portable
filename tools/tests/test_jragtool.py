#!/usr/bin/env python3
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS_DIR))

import jragtool


class JRAGToolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.qci = self.root / "ragdoll.qci"
        self.smd = self.root / "ragdoll.smd"
        self.jrag = self.root / "scientist.jrag"

        self.qci.write_text(
            '$collisionjoints ragdoll {\n'
            '    $mass 115\n'
            '    $inertia 10\n'
            '    $damping 0.01\n'
            '    $rotdamping 1.5\n'
            '    $rootbone "Bip01"\n'
            '    $jointconstrain "Bip01 L Leg" x limit -25 20\n'
            '    $jointconstrain "Bip01 Helper" y limit -5 5\n'
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

    def _run(self, args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = jragtool.main(args)
        return result, output.getvalue()

    def _generate(self):
        result, output = self._run([
            "generate", str(self.qci), "--smd", str(self.smd), "-o", str(self.jrag)
        ])
        self.assertEqual(result, 0, output)
        self.assertTrue(self.jrag.is_file())
        return output

    def test_inspect_reports_collision_hierarchy_and_qci(self):
        result, output = self._run([
            "inspect", str(self.smd), "--qci", str(self.qci)
        ])
        self.assertEqual(result, 0)
        self.assertIn("Skeleton bones: 3", output)
        self.assertIn("Collision bodies: 1", output)
        self.assertIn('Bip01 L Leg', output)
        self.assertIn("Root bone: Bip01", output)
        self.assertIn("Generated physical hierarchy: 1 bodies", output)

    def test_inspect_reports_repeated_smd_node_declarations(self):
        source = self.smd.read_text(encoding="utf-8")
        source = source.replace(
            '1 "Bip01 Helper" 0\n',
            '1 "Bip01 Helper" 0\n1 "Bip01 Helper" -1\n',
            1,
        )
        self.smd.write_text(source, encoding="utf-8")

        result, output = self._run(["inspect", str(self.smd)])
        self.assertEqual(result, 0, output)
        self.assertIn("repeated node index 1", output)
        self.assertIn("using final parent -1 instead of 0", output)

    def _add_helper_collision_mesh(self):
        source = self.smd.read_text(encoding="utf-8")
        local = [
            (-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
            (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1),
        ]
        world = [(-point[1] + 2, point[0], point[2]) for point in local]
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
                triangles.append("1 {} {} {} 0 0 1 0 0".format(x, y, z))
        self.smd.write_text(source + "\n" + "\n".join(triangles) + "\n", encoding="utf-8")

    def test_generate_runs_validation_and_writes_runtime_definition(self):
        output = self._generate()
        self.assertIn("Generated and validated", output)
        definition = jragtool.read_jrag(self.jrag)
        self.assertEqual(len(definition.bodies), 1)
        self.assertEqual(definition.bodies[0].name, "Bip01 L Leg")
        self.assertEqual(definition.bodies[0].joint_min, (-25.0, -1.0e30, -1.0e30))
        self.assertEqual(definition.bodies[0].angular_damping_override, 5.0)
        self.assertTrue(definition.self_collisions)
        self.assertEqual(definition.animated_friction, (0.0, 0.0, 0.0, 0.0, 0.0))
        self.assertIn("self_collisions 1", self.jrag.read_text(encoding="utf-8"))

    def test_generate_can_author_animated_joint_friction(self):
        result, output = self._run([
            "generate", str(self.qci), "--smd", str(self.smd),
            "-o", str(self.jrag),
            "--animated-friction", "10", "100", "0.4", "1.5", "0.8"
        ])
        self.assertEqual(result, 0, output)
        definition = jragtool.read_jrag(self.jrag)
        self.assertEqual(definition.animated_friction, (10.0, 100.0, 0.4, 1.5, 0.8))
        self.assertIn("animated_friction 10 100 0.4 1.5 0.8",
                      self.jrag.read_text(encoding="utf-8"))

    def test_generate_can_author_an_empty_collision_rule_allowlist(self):
        result, output = self._run([
            "generate", str(self.qci), "--smd", str(self.smd),
            "-o", str(self.jrag), "--collision-rules"
        ])
        self.assertEqual(result, 0, output)
        definition = jragtool.read_jrag(self.jrag)
        self.assertTrue(definition.collision_rules)
        self.assertEqual(definition.collision_pairs, ())
        self.assertIn("collision_rules 1", self.jrag.read_text(encoding="utf-8"))

    def test_generate_can_author_specific_collision_pairs(self):
        self._add_helper_collision_mesh()
        result, output = self._run([
            "generate", str(self.qci), "--smd", str(self.smd),
            "-o", str(self.jrag), "--collision-pair", "1", "0"
        ])
        self.assertEqual(result, 0, output)
        definition = jragtool.read_jrag(self.jrag)
        self.assertEqual(len(definition.bodies), 2)
        self.assertTrue(definition.collision_rules)
        self.assertEqual(definition.collision_pairs, ((0, 1),))
        self.assertIn("collision_rules 1\ncollision_pair 0 1",
                      self.jrag.read_text(encoding="utf-8"))

    def test_generate_rejects_collision_rules_with_disabled_self_collisions(self):
        result, output = self._run([
            "generate", str(self.qci), "--smd", str(self.smd),
            "-o", str(self.jrag), "--no-self-collisions", "--collision-rules"
        ])
        self.assertEqual(result, 2)
        self.assertIn("cannot be combined with self_collisions 0", output)

    def test_generate_can_disable_ragdoll_self_collisions(self):
        result, output = self._run([
            "generate", str(self.qci), "--smd", str(self.smd),
            "-o", str(self.jrag), "--no-self-collisions"
        ])
        self.assertEqual(result, 0, output)
        definition = jragtool.read_jrag(self.jrag)
        self.assertFalse(definition.self_collisions)
        self.assertIn("self_collisions 0", self.jrag.read_text(encoding="utf-8"))

    def test_legacy_jrag_without_optional_rules_uses_source_defaults(self):
        self._generate()
        contents = self.jrag.read_text(encoding="utf-8")
        contents = contents.replace("self_collisions 1\n", "", 1)
        definition = jragtool.parse_jrag_text(contents)
        self.assertTrue(definition.self_collisions)
        self.assertFalse(definition.collision_rules)
        self.assertEqual(definition.collision_pairs, ())
        self.assertEqual(definition.animated_friction, (0.0, 0.0, 0.0, 0.0, 0.0))

    def test_parser_accepts_and_normalizes_collision_pair_indices(self):
        self._add_helper_collision_mesh()
        self._generate()
        contents = self.jrag.read_text(encoding="utf-8")
        contents = contents.replace(
            "self_collisions 1\n",
            "self_collisions 1\ncollision_rules 1\ncollision_pair 1 0\n", 1)
        definition = jragtool.parse_jrag_text(contents)
        self.assertTrue(definition.collision_rules)
        self.assertEqual(definition.collision_pairs, ((0, 1),))

    def test_parser_rejects_invalid_collision_pair_rules(self):
        self._add_helper_collision_mesh()
        self._generate()
        base = self.jrag.read_text(encoding="utf-8").replace(
            "self_collisions 1\n", "self_collisions 1\ncollision_rules 1\n", 1)
        invalid_rules = (
            ("collision_pair 0 0\n", "distinct and non-negative"),
            ("collision_pair 0 2\n", "outside body_count"),
            ("collision_pair 0 1\ncollision_pair 1 0\n", "duplicate collision_pair"),
        )
        for rules, message in invalid_rules:
            with self.subTest(rules=rules):
                with self.assertRaisesRegex(ValueError, message):
                    jragtool.parse_jrag_text(base.replace("body_count 2\n", "body_count 2\n" + rules, 1))
        without_marker = self.jrag.read_text(encoding="utf-8").replace(
            "body_count 2\n", "collision_pair 0 1\nbody_count 2\n", 1)
        with self.assertRaisesRegex(ValueError, "requires collision_rules 1"):
            jragtool.parse_jrag_text(without_marker)
        conflict = base.replace("self_collisions 1", "self_collisions 0", 1)
        with self.assertRaisesRegex(ValueError, "cannot be combined"):
            jragtool.parse_jrag_text(conflict)

    def test_parser_rejects_invalid_animated_friction(self):
        self._generate()
        contents = self.jrag.read_text(encoding="utf-8")
        contents = contents.replace(
            "self_collisions 1\n",
            "animated_friction 20 10 0.4 1.5 0.8\nself_collisions 1\n",
            1,
        )
        with self.assertRaisesRegex(ValueError, "animated_friction values"):
            jragtool.parse_jrag_text(contents)

    def test_validate_with_qci_checks_geometry_and_authoring_values(self):
        self._generate()
        result, output = self._run([
            "validate", str(self.jrag), str(self.smd), "--qci", str(self.qci)
        ])
        self.assertEqual(result, 0, output)
        self.assertIn("Geometry, hull points, body order and physical parent links match", output)
        self.assertIn("Global physics settings and joint limits match", output)

    def test_validate_without_qci_still_checks_geometry(self):
        self._generate()
        result, output = self._run(["validate", str(self.jrag), str(self.smd)])
        self.assertEqual(result, 0, output)
        self.assertIn("Valid:", output)

    def test_validate_rejects_changed_hull_point(self):
        self._generate()
        contents = self.jrag.read_text(encoding="utf-8")
        self.jrag.write_text(contents.replace("point -1.000000 -1.000000 -1.000000",
                                              "point -0.900000 -1.000000 -1.000000", 1),
                             encoding="utf-8")
        result, output = self._run([
            "validate", str(self.jrag), str(self.smd), "--qci", str(self.qci)
        ])
        self.assertEqual(result, 2)
        self.assertIn("hull point", output)

    def test_validate_rejects_qci_joint_limit_drift(self):
        self._generate()
        contents = self.jrag.read_text(encoding="utf-8")
        self.jrag.write_text(contents.replace("joint_min -25 -1e+30 -1e+30",
                                              "joint_min -30 -1e+30 -1e+30", 1),
                             encoding="utf-8")
        result, output = self._run([
            "validate", str(self.jrag), str(self.smd), "--qci", str(self.qci)
        ])
        self.assertEqual(result, 2)
        self.assertIn("joint_min does not match", output)

    def test_nonphysical_qci_directive_is_warned_and_ignored(self):
        output = self._generate()
        self.assertIn(
            "WARNING: $jointconstrain targets non-physical skeleton bone 'Bip01 Helper'",
            output)
        definition = jragtool.read_jrag(self.jrag)
        self.assertEqual(definition.bodies[0].joint_min, (-25.0, -1.0e30, -1.0e30))
        self.assertEqual(definition.bodies[0].joint_max, (20.0, 1.0e30, 1.0e30))

    def test_unknown_qci_bone_remains_an_error(self):
        invalid_qci = self.root / "invalid.qci"
        invalid_qci.write_text(
            self.qci.read_text(encoding="utf-8").replace(
                '"Bip01 Helper"', '"NotInSkeleton"'),
            encoding="utf-8")
        result, output = self._run([
            "inspect", str(self.smd), "--qci", str(invalid_qci)
        ])
        self.assertEqual(result, 2)
        self.assertIn("was not found in the SMD nodes", output)

    def test_runtime_parser_rejects_trailing_directives(self):
        self._generate()
        contents = self.jrag.read_text(encoding="utf-8") + "unexpected 1\n"
        with self.assertRaisesRegex(ValueError, "unexpected data after end"):
            jragtool.parse_jrag_text(contents)


if __name__ == "__main__":
    unittest.main()
