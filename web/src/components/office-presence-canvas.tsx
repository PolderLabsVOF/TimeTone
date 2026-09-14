"use client";

import { useEffect, useRef } from "react";

type Person = { id: string; name: string; color: string; since: string };
type Node = Person & { x: number; y: number; vx: number; vy: number; r: number; phase: number; drift: number };

function seeded(index: number, salt: number) {
  const value = Math.sin((index + 1) * 12.9898 + salt * 78.233) * 43758.5453;
  return value - Math.floor(value);
}

function layoutNodes(count: number, width: number, height: number, radius: number) {
  const padding = radius + 18;
  const aspect = Math.max(0.8, width / Math.max(height, 1));
  const columns = Math.max(1, Math.ceil(Math.sqrt(count * aspect)));
  const rows = Math.ceil(count / columns);
  const cellWidth = (width - padding * 2) / columns;
  const cellHeight = (height - padding * 2) / rows;

  return Array.from({ length: count }, (_, index) => {
    const column = index % columns;
    const row = Math.floor(index / columns);
    return {
      x: padding + cellWidth * (column + 0.5 + (seeded(index, 1) - 0.5) * 0.42),
      y: padding + cellHeight * (row + 0.5 + (seeded(index, 2) - 0.5) * 0.42),
    };
  });
}

export function OfficePresenceCanvas({ people }: { people: Person[] }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const context = canvas.getContext("2d");
    if (!context) return;

    let frame = 0;
    let width = 1;
    let height = 1;
    let dpr = 1;
    let previous = performance.now();
    let laidOut = false;
    let dragging: Node | null = null;
    let selected: Node | null = null;
    let lastX = 0;
    let lastY = 0;
    const nodes: Node[] = people.map((person, index) => ({
      ...person,
      x: 0,
      y: 0,
      vx: 0,
      vy: 0,
      r: 24,
      phase: seeded(index, 3) * Math.PI * 2,
      drift: 0.55 + seeded(index, 4) * 0.55,
    }));

    const resize = () => {
      const rect = canvas.getBoundingClientRect();
      dpr = Math.min(window.devicePixelRatio || 1, 2);
      width = Math.max(1, rect.width);
      height = Math.max(1, rect.height);
      canvas.width = width * dpr;
      canvas.height = height * dpr;
      context.setTransform(dpr, 0, 0, dpr, 0, 0);

      const radius = Math.min(24, Math.max(18, width / 19));
      if (!laidOut) {
        const positions = layoutNodes(nodes.length, width, height, radius);
        nodes.forEach((node, index) => {
          node.r = radius;
          node.x = positions[index].x;
          node.y = positions[index].y;
        });
        laidOut = true;
      } else {
        for (const node of nodes) {
          node.r = radius;
          node.x = Math.max(radius, Math.min(width - radius, node.x));
          node.y = Math.max(radius, Math.min(height - radius - 14, node.y));
        }
      }
    };

    const render = (time: number) => {
      const step = Math.min(2, (time - previous) / 16.67);
      previous = time;
      const seconds = time / 1000;
      context.clearRect(0, 0, width, height);
      context.fillStyle = "#121a15";
      context.fillRect(0, 0, width, height);

      for (let a = 0; a < nodes.length; a++) {
        for (let b = a + 1; b < nodes.length; b++) {
          const first = nodes[a];
          const second = nodes[b];
          const dx = second.x - first.x;
          const dy = second.y - first.y;
          const distance = Math.max(0.001, Math.hypot(dx, dy));
          const desired = Math.max(first.r + second.r + 14, 92);
          if (distance < desired) {
            const force = (desired - distance) * 0.0018 * step;
            const pushX = dx / distance * force;
            const pushY = dy / distance * force;
            first.vx -= pushX;
            first.vy -= pushY;
            second.vx += pushX;
            second.vy += pushY;
          }
          if (distance < 135) {
            context.strokeStyle = `rgba(216,255,98,${(1 - distance / 135) * 0.07})`;
            context.lineWidth = 1;
            context.beginPath();
            context.moveTo(first.x, first.y);
            context.lineTo(second.x, second.y);
            context.stroke();
          }
        }
      }

      for (const node of nodes) {
        const wanderX = Math.cos(seconds * node.drift + node.phase) * 0.008;
        const wanderY = Math.sin(seconds * node.drift * 0.83 + node.phase * 1.7) * 0.008;
        node.vx += wanderX * step;
        node.vy += wanderY * step;

        const edgeMargin = node.r + 16;
        if (node.x < edgeMargin) node.vx += 0.009 * step;
        if (node.x > width - edgeMargin) node.vx -= 0.009 * step;
        if (node.y < edgeMargin) node.vy += 0.009 * step;
        if (node.y > height - edgeMargin - 14) node.vy -= 0.009 * step;

        node.vx *= 0.992;
        node.vy *= 0.992;
        const speed = Math.hypot(node.vx, node.vy);
        if (speed > 0.42) {
          node.vx = node.vx / speed * 0.42;
          node.vy = node.vy / speed * 0.42;
        }
        if (node !== dragging) {
          node.x += node.vx * step;
          node.y += node.vy * step;
        }
        node.x = Math.max(node.r, Math.min(width - node.r, node.x));
        node.y = Math.max(node.r, Math.min(height - node.r - 14, node.y));

        context.beginPath();
        context.fillStyle = `${node.color}12`;
        context.arc(node.x, node.y, node.r + 6, 0, Math.PI * 2);
        context.fill();
        if (node === selected) {
          context.beginPath();
          context.strokeStyle = "rgba(216,255,98,.55)";
          context.lineWidth = 1.5;
          context.arc(node.x, node.y, node.r + 5, 0, Math.PI * 2);
          context.stroke();
        }
        context.beginPath();
        context.fillStyle = node.color;
        context.globalAlpha = 0.82;
        context.arc(node.x, node.y, node.r, 0, Math.PI * 2);
        context.fill();
        context.globalAlpha = 1;
        context.fillStyle = "#17211b";
        context.font = "700 12px system-ui";
        context.textAlign = "center";
        context.textBaseline = "middle";
        context.fillText(node.name.split(" ").map((part) => part[0]).join("").slice(0, 2).toUpperCase(), node.x, node.y + 1);
        context.fillStyle = "rgba(237,245,238,.58)";
        context.font = "500 9px system-ui";
        const label = node.name.length > 16 ? `${node.name.slice(0, 15)}…` : node.name;
        context.fillText(label, node.x, Math.min(height - 5, node.y + node.r + 12));
      }
      frame = requestAnimationFrame(render);
    };

    const point = (event: PointerEvent) => {
      const rect = canvas.getBoundingClientRect();
      return { x: event.clientX - rect.left, y: event.clientY - rect.top };
    };
    const pointerDown = (event: PointerEvent) => {
      const { x, y } = point(event);
      dragging = [...nodes].reverse().find((node) => Math.hypot(node.x - x, node.y - y) <= node.r + 10) || null;
      if (!dragging) return;
      selected = dragging;
      lastX = x;
      lastY = y;
      canvas.focus();
      canvas.setPointerCapture(event.pointerId);
    };
    const pointerMove = (event: PointerEvent) => {
      if (!dragging) return;
      const { x, y } = point(event);
      dragging.vx = (x - lastX) * 0.22;
      dragging.vy = (y - lastY) * 0.22;
      dragging.x = x;
      dragging.y = y;
      lastX = x;
      lastY = y;
    };
    const pointerUp = (event: PointerEvent) => {
      if (dragging && canvas.hasPointerCapture(event.pointerId)) canvas.releasePointerCapture(event.pointerId);
      dragging = null;
    };
    const keyDown = (event: KeyboardEvent) => {
      if (!selected) return;
      const distance = event.shiftKey ? 18 : 8;
      const moves: Record<string, [number, number]> = { ArrowUp: [0, -distance], ArrowDown: [0, distance], ArrowLeft: [-distance, 0], ArrowRight: [distance, 0] };
      const move = moves[event.key];
      if (!move) return;
      event.preventDefault();
      selected.x = Math.max(selected.r, Math.min(width - selected.r, selected.x + move[0]));
      selected.y = Math.max(selected.r, Math.min(height - selected.r - 14, selected.y + move[1]));
      selected.vx = 0;
      selected.vy = 0;
    };

    const observer = new ResizeObserver(resize);
    observer.observe(canvas);
    resize();
    frame = requestAnimationFrame(render);
    canvas.addEventListener("pointerdown", pointerDown);
    canvas.addEventListener("pointermove", pointerMove);
    canvas.addEventListener("pointerup", pointerUp);
    canvas.addEventListener("pointercancel", pointerUp);
    canvas.addEventListener("keydown", keyDown);
    return () => {
      cancelAnimationFrame(frame);
      observer.disconnect();
      canvas.removeEventListener("pointerdown", pointerDown);
      canvas.removeEventListener("pointermove", pointerMove);
      canvas.removeEventListener("pointerup", pointerUp);
      canvas.removeEventListener("pointercancel", pointerUp);
      canvas.removeEventListener("keydown", keyDown);
    };
  }, [people]);

  if (!people.length) return <div className="grid h-56 place-items-center rounded-2xl border border-dashed border-white/15 text-center text-sm text-white/45">The nodes come alive when someone checks in.</div>;
  return <canvas ref={canvasRef} role="application" tabIndex={0} aria-label="Interactive office presence map; drag a person to move them, or focus the map and use arrow keys" className="mt-5 h-56 w-full cursor-grab touch-none select-none rounded-2xl bg-[#121a15] outline-none focus-visible:ring-2 focus-visible:ring-[#d8ff62]/55 active:cursor-grabbing" />;
}
