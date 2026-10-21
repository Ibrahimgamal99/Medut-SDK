"""Start and stop the installed wheel's core through the binding's config.

Run by .github/workflows/pypi.yml on each wheel's own platform. medut_init()
reads medut_config_t at the offsets the core was compiled with, so a binding
header that lays the struct out differently fails here with -1, as every
medut 1.0.0 wheel did.
"""

from medut._loader import ffi, lib


@ffi.callback("void(const medut_event_t *, void *)")
def _on_event(ev, userdata):
    pass


cfg = ffi.new("medut_config_t *")
lib.medut_config_init(cfg)
cfg.event_cb = _on_event
rc = lib.medut_init(cfg)
if rc != 0:
    raise SystemExit(f"medut_init returned {rc}")
lib.medut_shutdown()
print("medut_init / medut_shutdown OK")
