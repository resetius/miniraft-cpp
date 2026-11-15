"""Real CLI restart smoke test; deterministic fault schedules live in test_snapshots."""
import pathlib
import resource
import socket
import struct
import subprocess
import sys
import tempfile
import time


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def receive(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        assert chunk, "server closed the connection"
        data.extend(chunk)
    return bytes(data)


def request(sock, key, value=None, flags=None):
    if value is None:
        frame = struct.pack("<IIIIH", 8, 24 + len(key), 4 if flags is None else flags, 17, len(key)) + key + bytes(6)
    else:
        frame = struct.pack("<IIIIHH", 8, 24 + len(key) + len(value), 1, 17, len(key), len(value)) + key + value + bytes(4)
    sock.sendall(frame)
    kind, length = struct.unpack("<II", receive(sock, 8))
    assert kind == 9 and 24 <= length <= 16 * 1024 * 1024
    body = receive(sock, length - 8)
    index, cookie, error = struct.unpack("<QII", body[:16])
    assert cookie == 17 and error == 0, (cookie, error)
    return index, body[16:]


def snapshot_boundary(path):
    data = path.read_bytes()
    if data[:8] == b"MRFTD002":
        length_offset = 40
    elif data[:8] == b"MRFTD003":
        voters = struct.unpack("<I", data[40:44])[0]
        assert 0 < voters <= 4096
        length_offset = 44 + 4 * voters
    elif data[:8] == b"MRFTD004":
        voters = struct.unpack("<I", data[48:52])[0]
        assert voters <= 4096
        length_offset = 52 + 4 * voters
    else:
        return 0
    length = struct.unpack("<Q", data[length_offset:length_offset + 8])[0]
    if not length:
        return 0
    return struct.unpack("<Q", data[length_offset + 16:length_offset + 24])[0]


def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="miniraft-checkpoint-cli-") as directory:
        root = pathlib.Path(directory)
        process = None
        with (root / "output.log").open("w+") as log:
            def launch(extra=(), periodic=True):
                endpoint = port()
                args = [binary, "--server", "--id", "1", "--node", f"127.0.0.1:{endpoint}:1", "--durable", *(["--snapshots", "1"] if periodic else []), *extra]
                child = subprocess.Popen(args, cwd=root, stdout=log, stderr=log, preexec_fn=no_core)
                return child, endpoint

            def connect(child, endpoint):
                end = time.monotonic() + 8
                while time.monotonic() < end:
                    assert child.poll() is None, "server exited during startup"
                    try:
                        sock = socket.create_connection(("127.0.0.1", endpoint), timeout=2)
                        sock.settimeout(5)
                        return sock
                    except ConnectionRefusedError:
                        time.sleep(0.02)
                raise AssertionError("server did not listen")

            def stop(child):
                child.terminate()
                child.wait(timeout=5)

            try:
                for args in (["--snapshots", "1"], ["--server", "--durable", "--snapshots", "0"],
                             ["--learner", "3"], ["--server", "--learner", "3"],
                             ["--server", "--durable", "--learner", "0"],
                             ["--server", "--durable", "--learner", "3junk"]):
                    rejected = subprocess.run([binary, *args], cwd=root, stdout=log, stderr=log, timeout=5)
                    assert rejected.returncode != 0
                process, endpoint = launch()
                with connect(process, endpoint) as sock:
                    index, _ = request(sock, b"a", b"persisted")
                image = root / "state.snapshot.1"
                end = time.monotonic() + 8
                while time.monotonic() < end:
                    if image.exists() and struct.unpack("<Q", image.read_bytes()[8:16])[0] >= index:
                        break
                    time.sleep(0.02)
                else:
                    raise AssertionError("checkpoint was not published")
                assert snapshot_boundary(root / "state.durable.1") >= index, "compacted durable image was not published"
                stop(process)
                process, endpoint = launch(periodic=False)
                with connect(process, endpoint) as sock:
                    _, value = request(sock, b"a")
                    assert value == b"persisted", value
                stop(process)
                process, _ = launch(["--node", f"127.0.0.1:{port()}:2"])
                assert process.wait(timeout=5) != 0, "changed membership must reject checkpoint"
                before = (root / "state.durable.1").read_bytes()
                damaged = bytearray(image.read_bytes())
                damaged[-1] ^= 1
                image.write_bytes(damaged)
                process, _ = launch()
                assert process.wait(timeout=5) != 0, "corrupt checkpoint must fail startup"
                assert (root / "state.durable.1").read_bytes() == before
                log.flush()
                log.seek(0)
                output = log.read()
                assert "ERROR: AddressSanitizer" not in output and "runtime error:" not in output, output
                print("KV checkpoint CLI: save, restart/read, configuration and corruption rejection passed")
            except Exception:
                log.flush()
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
