import { useEffect, useRef, useState } from "react";
import gsap from "gsap";
import { ScrollTrigger } from "gsap/ScrollTrigger";
gsap.registerPlugin(ScrollTrigger);
export const clamp = (n: number) => Math.min(1, Math.max(0, n));
export const range = (p: number, a: number, b: number) =>
  clamp((p - a) / (b - a));
export const smooth = (n: number) => n * n * (3 - 2 * n);
export const mix = (a: number, b: number, p: number) => a + (b - a) * clamp(p);
export function useScene(final = 1) {
  const ref = useRef<HTMLElement>(null);
  const [progress, setProgress] = useState(0);
  const [reduced, setReduced] = useState(false);
  const [playing, setPlaying] = useState(false);
  const playback = useRef<gsap.core.Tween | null>(null);
  const value = useRef({ p: 0 });
  useEffect(() => {
    const mm = gsap.matchMedia();
    mm.add("(prefers-reduced-motion: reduce)", () => {
      playback.current?.kill();
      playback.current = null;
      setPlaying(false);
      setReduced(true);
      setProgress(final);
    });
    mm.add("(prefers-reduced-motion: no-preference)", () => {
      setReduced(false);
      gsap.to(value.current, {
        p: 1,
        ease: "none",
        onUpdate: () => {
          if (!playback.current) setProgress(value.current.p);
        },
        scrollTrigger: {
          trigger: ref.current,
          start: "top top",
          end: () =>
            `+=${Math.max(160, (ref.current?.offsetHeight ?? 0) - window.innerHeight)}`,
          scrub: 0.45,
          invalidateOnRefresh: true,
        },
      });
    });
    return () => {
      mm.revert();
      playback.current?.kill();
      playback.current = null;
    };
  }, [final]);
  const play = () => {
    if (playing) {
      playback.current?.pause();
      setPlaying(false);
      return;
    }
    if (playback.current && playback.current.progress() < 1) {
      playback.current.resume();
      setPlaying(true);
      return;
    }
    const demo = { p: 0 };
    setPlaying(true);
    playback.current = gsap.to(demo, {
      p: 1,
      duration: 10,
      ease: "none",
      onUpdate: () => setProgress(demo.p),
      onComplete: () => {
        setPlaying(false);
      },
    });
  };
  // Scrolling after a replay gives control back to the scroll position.
  useEffect(() => {
    const stop = () => {
      if (playback.current) {
        playback.current.kill();
        playback.current = null;
        setPlaying(false);
        setProgress(value.current.p);
      }
    };
    window.addEventListener("wheel", stop, { passive: true });
    window.addEventListener("touchmove", stop, { passive: true });
    const key = (e: KeyboardEvent) => {
      if (
        [
          "ArrowDown",
          "ArrowUp",
          "PageDown",
          "PageUp",
          "Home",
          "End",
          " ",
        ].includes(e.key)
      )
        stop();
    };
    window.addEventListener("keydown", key);
    return () => {
      window.removeEventListener("wheel", stop);
      window.removeEventListener("touchmove", stop);
      window.removeEventListener("keydown", key);
    };
  }, []);
  return { ref, progress, reduced, playing, play };
}
export function point(p: number, frames: number[][]) {
  let i = 1;
  while (i < frames.length - 1 && p > frames[i][0]) i++;
  const a = frames[i - 1],
    b = frames[i],
    t = smooth(range(p, a[0], b[0]));
  return { left: mix(a[1], b[1], t), top: mix(a[2], b[2], t) };
}
