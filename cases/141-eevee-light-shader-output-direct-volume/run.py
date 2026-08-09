from importlib import util
from pathlib import Path
import sys


CASE_DIR = Path(__file__).resolve().parent
ROOT = CASE_DIR.parents[3]
OUT_DIR = CASE_DIR / "out"
SCRIPTS_DIR = ROOT / "test" / "eevee_light_shader_output" / "scripts"


def load_validation_module(name, path):
    spec = util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to load validation module: {path}")
    module = util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    module.OUT_DIR = OUT_DIR / name
    module.OUT_DIR.mkdir(parents=True, exist_ok=True)
    return module


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    surface = load_validation_module(
        "surface",
        SCRIPTS_DIR / "validate_eevee_light_shader_output.py",
    )
    volume = load_validation_module(
        "volume",
        SCRIPTS_DIR / "validate_eevee_light_shader_volume.py",
    )

    surface.main()
    volume.main()
    print("EEVEE_LIGHT_SHADER_RELEASE_DIRECT_VOLUME_OK", flush=True)


if __name__ == "__main__":
    main()
