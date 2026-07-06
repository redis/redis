set python_cmd [auto_execok python3]
if {$python_cmd eq ""} {
    set python_cmd [auto_execok python]
}

if {$python_cmd ne ""} {
    tags {"bitmap" "bitmap-native"} {
        test {bitmap benchmark native helpers use public creation controls} {
            set code {
import importlib.util
import pathlib
import types

spec = importlib.util.spec_from_file_location(
    "bitmap_bench", pathlib.Path("tools/bitmap-bench.py")
)
bitmap_bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bitmap_bench)


class FakeClient:
    def __init__(self, default="no", strlen=4, bit=1):
        self.default = default
        self.strlen = strlen
        self.bit = bit
        self.commands = []

    def execute(self, cmd):
        cmd = list(cmd)
        self.commands.append(cmd)
        name = str(cmd[0]).upper()
        if name == "CONFIG" and str(cmd[1]).upper() == "GET":
            return [b"bitmap-default-roaring", self.default.encode()]
        if name == "CONFIG" and str(cmd[1]).upper() == "SET":
            self.default = str(cmd[3])
            return "OK"
        if name == "STRLEN":
            return self.strlen
        if name == "GETBIT":
            return self.bit
        if name == "SETBIT":
            return 0
        if name == "DEL":
            return 1
        if name == "SET":
            return "OK"
        raise AssertionError(f"unexpected command: {cmd!r}")


def make_bench(mode, client):
    bench = object.__new__(bitmap_bench.RedisBitmapBench)
    bench.args = types.SimpleNamespace(mode=mode)
    bench.client = client
    return bench


def assert_no_bitmap_command(client):
    assert not any(str(cmd[0]).upper() == "BITMAP" for cmd in client.commands), client.commands


client = FakeClient(default="yes", strlen=8, bit=1)
make_bench("native", client).convert_to_native("bench:key")
assert_no_bitmap_command(client)
assert client.default == "yes", client.commands
assert ["SETBIT", "bench:key", "0", "1"] in client.commands, client.commands

client = FakeClient(default="no", strlen=0, bit=0)
make_bench("native", client).convert_to_native("bench:empty")
assert_no_bitmap_command(client)
assert client.default == "no", client.commands
assert not any(str(cmd[0]).upper() == "SETBIT" for cmd in client.commands), client.commands

client = FakeClient(default="no")
make_bench("native", client).setup_bitfield_native_write()
assert_no_bitmap_command(client)
assert client.default == "no", client.commands
assert ["SETBIT", "bench:bitmap:bitfield:write", "0", "0"] in client.commands, client.commands

client = FakeClient(default="no")
make_bench("legacy", client).setup_bitfield_native_write()
assert_no_bitmap_command(client)
assert client.default == "no", client.commands
assert not any(str(cmd[0]).upper() == "SETBIT" for cmd in client.commands), client.commands
assert ["SET", "bench:bitmap:bitfield:write", b""] in client.commands, client.commands
            }
            exec {*}$python_cmd -c $code
        }
    }
}
