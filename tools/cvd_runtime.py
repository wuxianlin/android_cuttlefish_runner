#!/usr/bin/env python3

import json
import os
import shlex
import sys
from pathlib import Path


def find_config(home: Path, instance_name: str) -> Path:
    configured = os.environ.get("CUTTLEFISH_CONFIG_PATH")
    candidates = [
        Path(configured) if configured else None,
        home / "cuttlefish" / "instances" / instance_name / "cuttlefish_config.json",
        home / "instances" / instance_name / "cuttlefish_config.json",
    ]
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate
    matches = list(home.glob(f"**/instances/{instance_name}/cuttlefish_config.json"))
    if len(matches) == 1:
        return matches[0]
    raise RuntimeError(f"Unable to locate cuttlefish_config.json for {instance_name}")


def assignment(name: str, value: object) -> str:
    return f"{name}={shlex.quote(str(value))}"


def main() -> None:
    if len(sys.argv) != 3:
        raise RuntimeError(f"Usage: {sys.argv[0]} CF_HOME INSTANCE_NUMBER")
    home = Path(sys.argv[1]).resolve()
    instance_number = sys.argv[2]
    instance_name = f"cvd-{instance_number}"
    config_path = find_config(home, instance_name)
    with config_path.open() as source:
        config = json.load(source)
    instance = config["instances"][instance_number]
    display = instance["display_configs"][0]
    frame_socket = Path(instance["frame_sock_path"])
    values = {
        "CF_CONFIG_PATH": config_path,
        "CF_FRAME_SOCKET": frame_socket,
        "CF_INPUT_SOCKET_DIR": frame_socket.parent,
        "CF_VNC_DISPLAY": instance["qemu_vnc_server_port"],
        "CF_DISPLAY_WIDTH": display["x_res"],
        "CF_DISPLAY_HEIGHT": display["y_res"],
    }
    print("\n".join(assignment(name, value) for name, value in values.items()))


if __name__ == "__main__":
    main()
