"""Extract the runtime-compiled HLSL templates from the two title-screen layers, fill them the way the .cpp files
do, and compile them with D3DCompile (d3dcompiler_47.dll) so a shader error is caught before a build and a game
launch. Also checks that each filled source fits its layer's fixed buffer (kShaderSourceCapacity): a source that
does not fit is only logged at runtime (stage=shader result=fail reason=source) and the layer never draws.

Run from anywhere: python tools/title_screen/compile_check.py. Exit status 0 when both shaders compile and fit.
Keep the argument order of each fill in step with the snprintf call in the .cpp when a template gains a slot."""
import ctypes, re, sys
from pathlib import Path

SRC = str(Path(__file__).resolve().parents[2] / 'Dawn/src/client/hooks/graphics/renderer') + '/'


def template_of(path):
    s = open(path, encoding='utf-8').read()
    m = re.search(r'kShaderTemplate = R"\((.*?)\)";', s, re.S)
    return s, m.group(1)


def constant(s, name):
    m = re.search(r'constexpr (?:float|int|UINT) ' + name + r' = ([-\d.]+)F?;', s)
    return float(m.group(1))


def compile_hlsl(name, source):
    d3d = ctypes.WinDLL('d3dcompiler_47')
    code = ctypes.c_void_p(); msgs = ctypes.c_void_p()
    src = source.encode()
    hr = d3d.D3DCompile(src, len(src), name.encode(), None, None, b'main', b'ps_4_0', 0, 0, ctypes.byref(code), ctypes.byref(msgs))

    def blob_text(blob):
        if not blob:
            return ''
        vt = ctypes.cast(blob, ctypes.POINTER(ctypes.c_void_p)).contents.value
        fn_ptr = ctypes.cast(vt, ctypes.POINTER(ctypes.c_void_p))[3]      # ID3DBlob::GetBufferPointer
        fn_size = ctypes.cast(vt, ctypes.POINTER(ctypes.c_void_p))[4]     # ID3DBlob::GetBufferSize
        GetPtr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(fn_ptr)
        GetSize = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(fn_size)
        p = GetPtr(blob); n = GetSize(blob)
        return ctypes.string_at(p, n).decode(errors='replace')

    text = blob_text(msgs)
    ok = hr == 0 and code.value
    print(f'{name}: hr=0x{hr & 0xFFFFFFFF:08X} {"OK" if ok else "FAIL"} {text.strip()[:600]}')
    return ok


s, tpl = template_of(SRC + 'graphics_splash_invert.cpp')
gain = constant(s, 'kWashGain')
filled = tpl % (1,
                constant(s, 'kBootLineStart'), constant(s, 'kClosingLineStart'),
                constant(s, 'kBootLineFull'), constant(s, 'kClosingLineFull'),
                8 / 255 * gain, 89 / 255 * gain, 242 / 255 * gain,
                constant(s, 'kBootTintRed') * constant(s, 'kBootGain'), constant(s, 'kBootTintGreen') * constant(s, 'kBootGain'),
                constant(s, 'kBootTintBlue') * constant(s, 'kBootGain'),
                constant(s, 'kClosingInvertedGain'))
ok1 = compile_hlsl('dawn_splash_invert', filled)

s2, tpl2 = template_of(SRC + 'graphics_title_filigree.cpp')
filled2 = tpl2 % (1, 2, 3, 1, 0, int(constant(s2, 'kBudget')))
ok2 = compile_hlsl('dawn_title_filigree', filled2)
# The DLL fills each template into a fixed buffer: a source that does not fit is logged and the layer never draws.
ok3 = True
for name, src, filled_text in (('splash', s, filled), ('filigree', s2, filled2)):
    cap = int(re.search(r'kShaderSourceCapacity = (\d+);', src).group(1))
    fits = len(filled_text) < cap
    print(f'{name}: filled source {len(filled_text)} bytes, capacity {cap}: {"fits" if fits else "TOO LARGE"}')
    ok3 = ok3 and fits
sys.exit(0 if ok1 and ok2 and ok3 else 1)
