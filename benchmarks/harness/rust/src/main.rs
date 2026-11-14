use peppi::io::{peppi as pp, slippi};
use serde_json::json;
use std::{
    io::{self, BufRead, Cursor, Write},
    panic::{catch_unwind, AssertUnwindSafe},
    sync::atomic::{AtomicUsize, Ordering},
    time::Instant,
};

type Data = Result<Vec<u8>, String>;

fn parallel<F>(inputs: &[Vec<u8>], threads: usize, level: i32, f: F) -> Vec<Data>
where
    F: Fn(
            &[u8],
            &mut slpz::Compressor,
            &mut slpz::Decompressor,
            &mut zstd::bulk::Compressor<'_>,
            &mut zstd::bulk::Decompressor<'_>,
        ) -> Data
        + Sync,
{
    let next = AtomicUsize::new(0);
    std::thread::scope(|scope| {
        let work = || {
            let mut c = slpz::Compressor::new(level).unwrap();
            let mut d = slpz::Decompressor::new().unwrap();
            let mut zc = zstd::bulk::Compressor::new(level).unwrap();
            let mut zd = zstd::bulk::Decompressor::new().unwrap();
            let mut rows = Vec::new();
            loop {
                let i = next.fetch_add(1, Ordering::Relaxed);
                if i >= inputs.len() {
                    break;
                }
                let result = catch_unwind(AssertUnwindSafe(|| {
                    f(&inputs[i], &mut c, &mut d, &mut zc, &mut zd)
                }))
                .unwrap_or_else(|_| Err("codec panicked".into()));
                rows.push((i, result));
            }
            rows
        };
        let mut handles = Vec::new();
        for _ in 1..threads {
            handles.push(scope.spawn(work));
        }
        let mut rows = work();
        for h in handles {
            rows.extend(h.join().unwrap());
        }
        rows.sort_by_key(|r| r.0);
        rows.into_iter().map(|r| r.1).collect()
    })
}

fn main() {
    let args: Vec<_> = std::env::args().collect();
    assert_eq!(args.len(), 3, "manifest mode");
    let mode = &args[2];
    let inputs: Vec<Vec<u8>> = std::fs::read_to_string(&args[1])
        .unwrap()
        .lines()
        .map(|p| std::fs::read(p).unwrap())
        .collect();
    println!("{}", json!({"ready":true,"files":inputs.len()}));
    io::stdout().flush().unwrap();
    for command in io::stdin().lock().lines() {
        let command = command.unwrap();
        let values: Vec<_> = command.split_whitespace().collect();
        let level: i32 = values[0].parse().unwrap();
        let threads: usize = values[1].parse().unwrap();
        assert!((1..=256).contains(&threads));
        let start = Instant::now();
        let encoded = parallel(&inputs, threads, level, |bytes, c, _, zc, _| {
            if mode == "slpz" {
                return slpz::compress(c, bytes).map_err(|e| e.to_string());
            }
            let game = slippi::read(Cursor::new(bytes), None).map_err(|e| e.to_string())?;
            let compression = match mode.as_str() {
                "peppi-zstd" => Some(arrow2::io::ipc::write::Compression::ZSTD),
                "peppi-lz4" => Some(arrow2::io::ipc::write::Compression::LZ4),
                _ => None,
            };
            let mut out = Vec::new();
            pp::write(&mut out, game, Some(&pp::ser::Opts { compression }))
                .map_err(|e| e.to_string())?;
            if mode == "peppi-outer" {
                out = zc.compress(&out).map_err(|e| e.to_string())?;
            }
            Ok(out)
        });
        let enc_seconds = start.elapsed().as_secs_f64();
        let mut errors = Vec::new();
        let compressed: Vec<_> = encoded
            .into_iter()
            .map(|r| match r {
                Ok(b) => {
                    errors.push(String::new());
                    b
                }
                Err(e) => {
                    errors.push(e);
                    Vec::new()
                }
            })
            .collect();
        let start = Instant::now();
        let decoded = parallel(&compressed, threads, level, |bytes, _, d, _, zd| {
            if bytes.is_empty() {
                return Err("encoding failed".into());
            }
            if mode == "slpz" {
                return slpz::decompress(d, bytes).map_err(|e| e.to_string());
            }
            let outer;
            let bytes = if mode == "peppi-outer" {
                let size = zstd::zstd_safe::get_frame_content_size(bytes)
                    .map_err(|e| format!("invalid Zstd frame: {e:?}"))?
                    .ok_or("Zstd frame has no content size")?;
                let size = usize::try_from(size).map_err(|e| e.to_string())?;
                outer = zd.decompress(bytes, size).map_err(|e| e.to_string())?;
                &outer[..]
            } else {
                bytes
            };
            let game = pp::read(Cursor::new(bytes), None).map_err(|e| e.to_string())?;
            let mut out = Vec::new();
            slippi::write(&mut out, &game).map_err(|e| e.to_string())?;
            Ok(out)
        });
        let dec_seconds = start.elapsed().as_secs_f64();
        let rows: Vec<_> = decoded
            .into_iter()
            .enumerate()
            .map(|(i, result)| {
                if errors[i].is_empty() {
                    errors[i] = match result {
                        Ok(bytes) if bytes == inputs[i] => String::new(),
                        Ok(_) => "round-trip bytes differ".into(),
                        Err(e) => e,
                    };
                }
                json!({"bytes":compressed[i].len(),"error":errors[i]})
            })
            .collect();
        println!(
            "{}",
            json!({"encode_seconds":enc_seconds,
                             "decode_seconds":dec_seconds,"files":rows})
        );
        io::stdout().flush().unwrap();
    }
}
