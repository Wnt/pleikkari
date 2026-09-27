#!/usr/bin/env python3
# PLE-766: compile and link the Go's GLSL ES 3.00 shaders through the host's Mesa
# (surfaceless EGL, GLES 3), so precision and link errors fail the gate on the
# build box instead of on the Go. Mesa is stricter than no driver, not identical
# to the Adreno 540's: a pass here is necessary, not sufficient.
#
# Programs come from the sources themselves: every link(kVs, kFs) call in
# vr-environment.cpp (both arms of a ternary), the VrUi pair in vr-ui-shaders.h
# and the cinema's vs + oes/plain pair. Exit 77 (ctest SKIP) when there is no
# usable Mesa EGL/GLES 3 on the host.
import ctypes
import ctypes.util
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV_CPP = os.path.join(ROOT, 'android/app/src/main/cpp/vr-environment.cpp')
UI_H = os.path.join(ROOT, 'android/app/src/vr/cpp/vr-ui-shaders.h')
CINEMA_CPP = os.path.join(ROOT, 'android/app/src/vr/cpp/vr-cinema.cpp')

RAW = re.compile(r'(?:const\s+)?char\s*\*\s*(?:const\s+)?(\w+)\s*=\s*R"\((.*?)\)"', re.S)
SKIP = 77


def raw_strings(path):
	with open(path) as f:
		return {m.group(1): m.group(2) for m in RAW.finditer(f.read())}


def programs():
	out = []
	env = raw_strings(ENV_CPP)
	with open(ENV_CPP) as f:
		src = f.read()
	calls = re.findall(r'\blink\((k\w+),\s*([^;]*?)\);', src)
	if not calls:
		raise SystemExit('no link(...) calls found in vr-environment.cpp; update ' + __file__)
	for vs, fs_expr in calls:
		for fs in re.findall(r'\bk\w+', fs_expr):
			out.append(('vr-environment ' + vs + '+' + fs, env[vs], env[fs]))
	ui = raw_strings(UI_H)
	out.append(('vr-ui VrUiVertexShader+VrUiFragmentShader', ui['VrUiVertexShader'], ui['VrUiFragmentShader']))
	cinema = raw_strings(CINEMA_CPP)
	for fs in ('oes', 'plain'):
		out.append(('vr-cinema vs+' + fs, cinema['vs'], cinema[fs]))
	return out


def lib(name):
	path = ctypes.util.find_library(name)
	if not path:
		print('SKIP: lib' + name + ' not found')
		sys.exit(SKIP)
	return ctypes.CDLL(path)


def gl_context():
	os.environ.setdefault('EGL_PLATFORM', 'surfaceless')
	egl = lib('EGL')
	gles = lib('GLESv2')
	egl.eglGetDisplay.restype = ctypes.c_void_p
	egl.eglGetProcAddress.restype = ctypes.c_void_p
	egl.eglGetProcAddress.argtypes = [ctypes.c_char_p]
	proc = egl.eglGetProcAddress(b'eglGetPlatformDisplayEXT')
	get_platform_display = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p)(proc) if proc else None
	EGL_PLATFORM_SURFACELESS_MESA = 0x31DD
	dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, None, None) if get_platform_display else None
	if not dpy:
		dpy = egl.eglGetDisplay(None)
	if not dpy or not egl.eglInitialize(ctypes.c_void_p(dpy), None, None):
		print('SKIP: eglInitialize failed (no Mesa EGL on this host)')
		sys.exit(SKIP)
	egl.eglBindAPI(0x30A0)  # EGL_OPENGL_ES_API
	cfg = ctypes.c_void_p()
	n = ctypes.c_int()
	attrs = (ctypes.c_int * 3)(0x3040, 0x40, 0x3038)  # EGL_RENDERABLE_TYPE = ES3, NONE
	egl.eglChooseConfig(ctypes.c_void_p(dpy), attrs, ctypes.byref(cfg), 1, ctypes.byref(n))
	ctx_attrs = (ctypes.c_int * 3)(0x3098, 3, 0x3038)  # EGL_CONTEXT_MAJOR_VERSION = 3
	egl.eglCreateContext.restype = ctypes.c_void_p
	ctx = egl.eglCreateContext(ctypes.c_void_p(dpy), cfg if n.value else None, None, ctx_attrs)
	if not ctx or not egl.eglMakeCurrent(ctypes.c_void_p(dpy), None, None, ctypes.c_void_p(ctx)):
		print('SKIP: no GLES 3 context (surfaceless) on this host')
		sys.exit(SKIP)
	gles.glGetString.restype = ctypes.c_char_p
	print('renderer:', gles.glGetString(0x1F01).decode(), '|', gles.glGetString(0x1F02).decode())
	return gles


def info_log(gles, obj, program):
	buf = ctypes.create_string_buffer(4096)
	(gles.glGetProgramInfoLog if program else gles.glGetShaderInfoLog)(obj, len(buf), None, buf)
	return buf.value.decode(errors='replace').strip()


def compile_shader(gles, kind, src):
	s = gles.glCreateShader(kind)
	b = ctypes.c_char_p(src.encode())
	gles.glShaderSource(s, 1, ctypes.byref(b), None)
	gles.glCompileShader(s)
	ok = ctypes.c_int()
	gles.glGetShaderiv(s, 0x8B81, ctypes.byref(ok))  # GL_COMPILE_STATUS
	return s, ok.value, info_log(gles, s, False)


def main():
	progs = programs()
	gles = gl_context()
	failures = 0
	for name, vs, fs in progs:
		v, vok, vlog = compile_shader(gles, 0x8B31, vs)  # GL_VERTEX_SHADER
		f, fok, flog = compile_shader(gles, 0x8B30, fs)  # GL_FRAGMENT_SHADER
		err = ''
		if not vok:
			err = 'vertex compile: ' + vlog
		elif not fok:
			err = 'fragment compile: ' + flog
		else:
			p = gles.glCreateProgram()
			gles.glAttachShader(p, v)
			gles.glAttachShader(p, f)
			gles.glLinkProgram(p)
			ok = ctypes.c_int()
			gles.glGetProgramiv(p, 0x8B82, ctypes.byref(ok))  # GL_LINK_STATUS
			if not ok.value:
				err = 'link: ' + info_log(gles, p, True)
		print(('FAIL ' if err else 'ok   ') + name + (('\n     ' + err) if err else ''))
		failures += bool(err)
	print('%d program(s), %d failing' % (len(progs), failures))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main())
