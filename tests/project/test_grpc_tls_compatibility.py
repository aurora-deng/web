"""Offline CMake dependency guards; no compiler, listener, DB, install or download."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "cmake/GrpcTlsCompatibility.cmake"


class GrpcTlsCompatibility(unittest.TestCase):
    def configure(self, targets):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake is not installed; no dependency was installed")
        with tempfile.TemporaryDirectory(prefix="grpc-tls-guard-") as temporary:
            source = Path(temporary)
            lines = ["cmake_minimum_required(VERSION 3.16)", "project(GrpcTlsGuard NONE)"]
            lines += [f"add_library({target} INTERFACE IMPORTED)" for target in targets]
            lines += [f'include("{MODULE.as_posix()}")', "webserver_require_compatible_grpc_tls()"]
            (source / "CMakeLists.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
            env = {key: os.environ[key] for key in ("PATH", "SYSTEMROOT", "WINDIR", "COMSPEC", "TEMP", "TMP", "LANG") if key in os.environ}
            return subprocess.run([cmake, "-S", str(source), "-B", str(source / "build"), "-G", "Ninja"],
                                  capture_output=True, text=True, env=env, timeout=30)

    def test_system_openssl_targets_are_accepted(self):
        result = self.configure(["OpenSSL::SSL", "OpenSSL::Crypto", "gRPC::grpc++"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_each_bundled_tls_target_is_rejected(self):
        for target in ("gRPC::SSL", "gRPC::Crypto", "gRPC::ssl", "gRPC::crypto"):
            with self.subTest(target=target):
                result = self.configure(["OpenSSL::SSL", "OpenSSL::Crypto", "gRPC::grpc++", target])
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Incompatible gRPC TLS provider", result.stderr)
                self.assertIn("gRPC_SSL_PROVIDER=package", result.stderr)

    def test_missing_native_tls_target_is_rejected(self):
        for targets in (["OpenSSL::SSL"], ["OpenSSL::Crypto"], []):
            with self.subTest(targets=targets):
                result = self.configure(targets)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Native TLS requires", result.stderr)

    def test_application_invokes_guard_immediately_after_package_lookup(self):
        text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8-sig")
        self.assertIn('find_package(gRPC CONFIG REQUIRED)\n    include("${PROJECT_SOURCE_DIR}/cmake/GrpcTlsCompatibility.cmake")\n    webserver_require_compatible_grpc_tls()', text)
        self.assertNotIn("SKIP_GRPC_TLS", MODULE.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
