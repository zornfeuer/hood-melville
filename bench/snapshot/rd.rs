// R читателей без писателя: каждый ITERS раз обходит копию одного снимка (как rd.c).
use hood_melville::SyncQueue;
const SNAP_SIZE: i32 = 4096;
const PAIRS: i32 = 300_000;
const ITERS: usize = 300;
#[allow(clippy::cast_sign_loss, reason = "как `(uint64_t)v` в rd.c")]
#[allow(
    clippy::cast_precision_loss,
    reason = "число обходов и наносекунды — в f64, как в rd.c"
)]
fn main() {
    let r: usize = std::env::args().nth(1).unwrap().parse().unwrap();
    let mut q = SyncQueue::new_sync();
    for i in 0..SNAP_SIZE {
        q.push_mut(i);
    }
    for k in 0..PAIRS {
        q.push_mut(SNAP_SIZE + k);
        q.pop_mut().unwrap();
    }
    let snap = q.clone();
    let t0 = std::time::Instant::now();
    let res: Vec<u64> = std::thread::scope(|s| {
        let hs: Vec<_> = (0..r)
            .map(|_| {
                s.spawn(|| {
                    let mut sum = 0u64;
                    for _ in 0..ITERS {
                        for v in snap.clone() {
                            sum = sum.wrapping_mul(31).wrapping_add(v as u64);
                        }
                    }
                    sum
                })
            })
            .collect();
        hs.into_iter().map(|h| h.join().unwrap()).collect()
    });
    let ns = t0.elapsed().as_nanos() as f64;
    println!(
        "{} {:.0}",
        res[0],
        (r * ITERS * SNAP_SIZE as usize) as f64 / ns * 1e3
    );
}
