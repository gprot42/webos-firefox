//! Does a 32-bit softfp Rust build work on the webOS TV, and how fast is its
//! floating point? Build it several ways (see build.sh) and compare.
//!
//! Correctness: call the TV's own libm with float arguments, and on the GPU
//! pass floats to glClearColor, render offscreen and read the pixel back.
//! A calling-convention mismatch shows up as wrong numbers or a wrong colour.
#[allow(unused_imports)]
use std::ffi::{c_char, c_int, c_void, CStr};
use std::hint::black_box;
use std::time::Instant;

// ---------------------------------------------------------------- benchmarks
fn nbody(steps: usize) -> f64 {
    const PI: f64 = std::f64::consts::PI;
    const SM: f64 = 4.0 * PI * PI;
    const DPY: f64 = 365.24;
    let mut b: [[f64; 7]; 5] = [
        [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, SM],
        [4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
         1.66007664274403694e-03 * DPY, 7.69901118419740425e-03 * DPY, -6.90460016972063023e-05 * DPY,
         9.54791938424326609e-04 * SM],
        [8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
         -2.76742510726862411e-03 * DPY, 4.99852801234917238e-03 * DPY, 2.30417297573763929e-05 * DPY,
         2.85885980666130812e-04 * SM],
        [1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
         2.96460137564761618e-03 * DPY, 2.37847173959480950e-03 * DPY, -2.96589568540237556e-05 * DPY,
         4.36624404335156298e-05 * SM],
        [1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
         2.68067772490389322e-03 * DPY, 1.62824170038242295e-03 * DPY, -9.51592254519715870e-05 * DPY,
         5.15138902046611451e-05 * SM],
    ];
    let dt = 0.01;
    for _ in 0..steps {
        for i in 0..5 {
            for j in (i + 1)..5 {
                let dx = b[i][0] - b[j][0];
                let dy = b[i][1] - b[j][1];
                let dz = b[i][2] - b[j][2];
                let d2 = dx * dx + dy * dy + dz * dz;
                let mag = dt / (d2 * d2.sqrt());
                let (mi, mj) = (b[i][6], b[j][6]);
                b[i][3] -= dx * mj * mag; b[i][4] -= dy * mj * mag; b[i][5] -= dz * mj * mag;
                b[j][3] += dx * mi * mag; b[j][4] += dy * mi * mag; b[j][5] += dz * mi * mag;
            }
        }
        for p in b.iter_mut() {
            p[0] += dt * p[3]; p[1] += dt * p[4]; p[2] += dt * p[5];
        }
    }
    b.iter().map(|p| p[0] + p[1] + p[2]).sum()
}

fn mandel(w: usize, h: usize, maxit: u32) -> u64 {
    let mut total = 0u64;
    for y in 0..h {
        for x in 0..w {
            let cr = black_box(x as f64 * 3.0 / w as f64 - 2.0);
            let ci = black_box(y as f64 * 2.0 / h as f64 - 1.0);
            let (mut zr, mut zi, mut i) = (0.0f64, 0.0f64, 0u32);
            while i < maxit && zr * zr + zi * zi < 4.0 {
                let t = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = t;
                i += 1;
            }
            total += i as u64;
        }
    }
    total
}

fn saxpy(n: usize, reps: usize) -> f32 {
    let x: Vec<f32> = (0..n).map(|i| (i % 97) as f32 * 0.01).collect();
    let mut y: Vec<f32> = (0..n).map(|i| (i % 89) as f32 * 0.02).collect();
    let a = black_box(0.999f32);
    for _ in 0..reps {
        for (yi, xi) in y.iter_mut().zip(&x) {
            *yi = a * *xi + *yi * 0.5;
        }
    }
    y.iter().sum()
}

// A real function boundary with float arguments and result: this is where
// softfp (floats in integer registers) and hardfp (floats in d0/d1) differ.
#[inline(never)]
extern "C" fn fcall(a: f64, b: f64) -> f64 {
    a * 1.000_000_1 + b * 0.999_999_9
}
fn abi_calls(n: usize) -> f64 {
    let mut acc = 0.5;
    for i in 0..n {
        acc = fcall(black_box(acc), i as f64 * 1e-9) * 0.5;
    }
    acc
}

fn libm_sin(n: usize) -> f64 {
    let f: fn(f64) -> f64 = black_box(f64::sin);
    let mut acc = 0.0;
    for i in 0..n {
        acc += black_box(f(black_box(i as f64 * 0.001)));
    }
    acc
}

fn time<T: std::fmt::Debug>(name: &str, f: impl Fn() -> T) {
    let t0 = Instant::now();
    let mut r = f();
    let mut best = t0.elapsed().as_secs_f64();
    if best < 1.5 {
        for _ in 0..2 {
            let t = Instant::now();
            r = f();
            best = best.min(t.elapsed().as_secs_f64());
        }
    }
    println!("BENCH {:<10} {:>9.1} ms   result {:?}", name, best * 1000.0, r);
}

// ------------------------------------------------ calls into the TV's libraries
#[cfg(target_env = "gnu")]
mod tv {
    use super::*;
    #[link(name = "m")]
    extern "C" {
        fn pow(x: f64, y: f64) -> f64;
        fn atan2(y: f64, x: f64) -> f64;
        fn expf(x: f32) -> f32;
        fn fmaf(x: f32, y: f32, z: f32) -> f32;
    }
    extern "C" {
        fn dlopen(name: *const c_char, flags: c_int) -> *mut c_void;
        fn dlsym(h: *mut c_void, name: *const c_char) -> *mut c_void;
        fn dlerror() -> *const c_char;
        fn gnu_get_libc_version() -> *const c_char;
    }
    const RTLD_NOW: c_int = 2;
    const RTLD_GLOBAL: c_int = 0x100;

    pub fn libm() {
        let libc = unsafe { CStr::from_ptr(gnu_get_libc_version()) }.to_string_lossy();
        println!("glibc at runtime: {libc}");
        let checks: [(&str, f64, f64); 4] = [
            ("pow(2, 0.5)", unsafe { pow(black_box(2.0), black_box(0.5)) }, 1.4142135623730951),
            ("atan2(1, 2)", unsafe { atan2(black_box(1.0), black_box(2.0)) }, 0.4636476090008061),
            ("expf(1.5)", unsafe { expf(black_box(1.5f32)) } as f64, 4.481689),
            ("fmaf(1.5,2,0.25)", unsafe { fmaf(black_box(1.5f32), 2.0, 0.25) } as f64, 3.25),
        ];
        let mut ok = true;
        for (name, got, want) in checks {
            let good = (got - want).abs() < 1e-5;
            ok &= good;
            println!("TVLIBM {:<16} = {:<20} expected {:<20} {}", name, got, want, if good { "ok" } else { "WRONG" });
        }
        println!("TVLIBM verdict: {}", if ok { "PASS" } else { "FAIL" });
    }

    unsafe fn open(names: &[&str]) -> *mut c_void {
        for n in names {
            let c = std::ffi::CString::new(*n).unwrap();
            let h = dlopen(c.as_ptr(), RTLD_NOW | RTLD_GLOBAL);
            if !h.is_null() {
                println!("GPU  loaded {n}");
                return h;
            }
        }
        println!("GPU  could not load {:?}: {}", names, CStr::from_ptr(dlerror()).to_string_lossy());
        std::ptr::null_mut()
    }
    unsafe fn sym<T>(h: *mut c_void, name: &str) -> T {
        let c = std::ffi::CString::new(name).unwrap();
        let p = dlsym(h, c.as_ptr());
        assert!(!p.is_null(), "missing symbol {name}");
        std::mem::transmute_copy(&p)
    }

    type P = *mut c_void;
    pub fn gpu() {
        unsafe {
            let wl = open(&["libwayland-client.so.0"]);
            let egl = open(&["libEGL.so.1", "libEGL.so"]);
            let gles = open(&["libGLESv2.so.2", "libGLESv2.so"]);
            if wl.is_null() || egl.is_null() || gles.is_null() { println!("GPU verdict: SKIP"); return; }
            let wl_connect: extern "C" fn(*const c_char) -> P = sym(wl, "wl_display_connect");
            let wl_disconnect: extern "C" fn(P) = sym(wl, "wl_display_disconnect");
            let get_display: extern "C" fn(P) -> P = sym(egl, "eglGetDisplay");
            let initialize: extern "C" fn(P, *mut i32, *mut i32) -> u32 = sym(egl, "eglInitialize");
            let query: extern "C" fn(P, i32) -> *const c_char = sym(egl, "eglQueryString");
            let bind_api: extern "C" fn(u32) -> u32 = sym(egl, "eglBindAPI");
            let choose: extern "C" fn(P, *const i32, *mut P, i32, *mut i32) -> u32 = sym(egl, "eglChooseConfig");
            let pbuffer: extern "C" fn(P, P, *const i32) -> P = sym(egl, "eglCreatePbufferSurface");
            let context: extern "C" fn(P, P, P, *const i32) -> P = sym(egl, "eglCreateContext");
            let make_current: extern "C" fn(P, P, P, P) -> u32 = sym(egl, "eglMakeCurrent");
            let get_error: extern "C" fn() -> i32 = sym(egl, "eglGetError");
            let terminate: extern "C" fn(P) -> u32 = sym(egl, "eglTerminate");
            let gl_string: extern "C" fn(u32) -> *const c_char = sym(gles, "glGetString");
            let clear_color: extern "C" fn(f32, f32, f32, f32) = sym(gles, "glClearColor");
            let clear: extern "C" fn(u32) = sym(gles, "glClear");
            let finish: extern "C" fn() = sym(gles, "glFinish");
            let read: extern "C" fn(i32, i32, i32, i32, u32, u32, *mut u8) = sym(gles, "glReadPixels");

            let conn = wl_connect(std::ptr::null());
            println!("GPU  wayland display {:?}", conn);
            let dpy = get_display(conn);
            let (mut maj, mut min) = (0, 0);
            if initialize(dpy, &mut maj, &mut min) == 0 {
                println!("GPU  eglInitialize failed 0x{:x}\nGPU verdict: FAIL", get_error());
                return;
            }
            let s = |k| CStr::from_ptr(query(dpy, k)).to_string_lossy().into_owned();
            println!("GPU  EGL {maj}.{min}, vendor {}, version {}", s(0x3053), s(0x3054));
            bind_api(0x30A0); // EGL_OPENGL_ES_API
            let attrs = [0x3033, 0x0001, 0x3040, 0x0004, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038];
            let mut cfg: P = std::ptr::null_mut();
            let mut n = 0;
            if choose(dpy, attrs.as_ptr(), &mut cfg, 1, &mut n) == 0 || n < 1 {
                println!("GPU  no pbuffer config 0x{:x}\nGPU verdict: FAIL", get_error());
                terminate(dpy); wl_disconnect(conn); return;
            }
            let surf = pbuffer(dpy, cfg, [0x3057, 16, 0x3056, 16, 0x3038].as_ptr());
            let ctx = context(dpy, cfg, std::ptr::null_mut(), [0x3098, 2, 0x3038].as_ptr());
            if surf.is_null() || ctx.is_null() || make_current(dpy, surf, surf, ctx) == 0 {
                println!("GPU  offscreen context failed 0x{:x}\nGPU verdict: FAIL", get_error());
                terminate(dpy); wl_disconnect(conn); return;
            }
            let g = |k| CStr::from_ptr(gl_string(k)).to_string_lossy().into_owned();
            println!("GPU  GL_RENDERER {}\nGPU  GL_VERSION  {}", g(0x1F01), g(0x1F02));
            // Floats into the TV's 32-bit GLES driver: the softfp ABI test.
            clear_color(black_box(0.25), black_box(0.5), black_box(0.75), black_box(1.0));
            clear(0x4000);
            finish();
            let mut px = [0u8; 4];
            read(8, 8, 1, 1, 0x1908, 0x1401, px.as_mut_ptr());
            let want = [64u8, 128, 191, 255];
            let good = px.iter().zip(want).all(|(a, b)| (*a as i32 - b as i32).abs() <= 1);
            println!("GPU  glClearColor(0.25,0.5,0.75,1) read back {:?}, expected about {:?}", px, want);
            println!("GPU verdict: {}", if good { "PASS" } else { "FAIL" });
            make_current(dpy, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut());
            terminate(dpy);
            wl_disconnect(conn);
        }
    }
}

fn main() {
    println!("BUILD {}", env!("PROBE_BUILD"));
    println!("pointer width {} bits, hard-float ABI {}, neon {}",
        usize::BITS, cfg!(target_abi = "eabihf") || cfg!(target_arch = "aarch64"),
        cfg!(target_feature = "neon"));
    #[cfg(target_env = "gnu")]
    {
        tv::libm();
        if std::env::args().any(|a| a == "--gpu") { tv::gpu(); }
    }
    time("nbody", || nbody(black_box(400_000)));
    time("mandel", || mandel(black_box(320), 240, 400));
    time("saxpy", || saxpy(black_box(1 << 20), 40));
    time("abi-calls", || abi_calls(black_box(20_000_000)));
    time("sin", || libm_sin(black_box(2_000_000)));
}
