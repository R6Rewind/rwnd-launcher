import {h, svg} from './dom';
import {rBlock} from './brand';

let mediaUrls: string[] = [];
export function setMedia(urls: string[]) { mediaUrls = [...urls ]; }

const all = new Set<MediaBackdrop>();
export function setVisible(visible: boolean) {
    for (const m of all) visible ? m.resume() : m.pause();
}

export class MediaBackdrop {
    readonly el: HTMLElement;
    readonly video: HTMLVideoElement;
    private playlist: string[];
    private index: number;
    private seaming = false;
    private fading?: Animation;

    constructor(private radius = 0) {
        this.video = h('video', {muted : true, playsInline : true, preload : 'auto', disablePictureInPicture : true}) as
                     HTMLVideoElement;
        this.video.muted = true;
        this.video.volume = 0;
        this.el = h('div.media', this.artwork(), this.video, h('div.vignette'));
        if (this.radius) this.el.style.borderRadius = this.radius + 'px';
        this.playlist = [...mediaUrls ];
        this.index = this.playlist.length ? Math.floor(Math.random() * this.playlist.length) : 0;
        this.video.addEventListener('playing', () => this.fade(1, 700));
        this.video.addEventListener('ended', () => this.seam());
        this.video.addEventListener('error', () => {
            if (!this.playlist.length) return;
            this.playlist.splice(this.index % this.playlist.length, 1);
            this.index = 0;
            this.video.style.opacity = '0';
            if (this.playlist.length) this.play();
        });
        all.add(this);
        if (this.playlist.length) queueMicrotask(() => this.play());
    }

    dispose() {
        all.delete(this);
        this.video.pause();
        this.video.removeAttribute('src');
        this.video.load();
    }
    pause() {
        if (this.video.src) this.video.pause();
    }
    resume() {
        if (this.video.src) this.video.play().catch(() => {});
    }

    private fade(to: number, ms: number, done?: () => void) {
        const from = parseFloat(getComputedStyle(this.video).opacity) || 0;
        this.fading?.cancel();
        this.video.style.opacity = String(to);
        this.fading = this.video.animate([ {opacity : from}, {opacity : to} ], {duration : ms, easing : 'linear'});
        if (done) this.fading.onfinish = done;
    }

    private seam() {
        if (this.seaming || !this.playlist.length) return;
        this.seaming = true;
        this.fade(0.15, 300, () => {
            if (this.playlist.length > 1) {
                this.index = (this.index + 1) % this.playlist.length;
                this.play();
            } else {
                this.video.currentTime = 0;
                this.video.play().catch(() => {});
                this.fade(1, 450);
            }
            this.seaming = false;
        });
    }

    private play() {
        this.video.src = this.playlist[this.index % this.playlist.length];
        this.video.play().catch(() => {});
    }

    private artwork() {
        const mark = h('div.mark');
        mark.append(svg(rBlock('#E8E4DA', '#F2A541')));
        return h('div.art', h('div.lines'), mark, h('div.leak'));
    }
}

const registered = (() => {
    try {
        for (const name of ['--bx', '--by'])
            CSS.registerProperty({name, syntax : '<percentage>', inherits : false, initialValue : '0%'});
    } catch {
    }
    return true;
})();

export class AmbientBackdrop {
    readonly el: HTMLElement;
    private canvas = h('canvas') as HTMLCanvasElement;
    private timer = 0;

    constructor(private mirror?: MediaBackdrop) {
        void registered;
        this.el = h('div.ambient');
        if (mirror) {
            this.canvas.width = 156;
            this.canvas.height = 106;
            this.el.append(this.canvas);
            this.timer = window.setInterval(() => this.draw(), 66);
        }
        this.el.append(this.bloom('242, 165, 65', 0x60, 0.72, 0.05, 0.55, 23),
                       this.bloom('62, 124, 177', 0x70, 0.05, 0.95, 0.65, 29),
                       this.bloom('194, 91, 42', 0x40, 0.95, 1.0, 0.5, 31), h('div.dim'));
    }

    dispose() { clearInterval(this.timer); }

    private bloom(rgb: string, alpha: number, cx: number, cy: number, radius: number, seconds: number) {
        const el = h('div.bloom');
        el.style.background = `radial-gradient(ellipse ${radius * 100}% ${radius * 120}% at var(--bx) var(--by), rgba(${
            rgb}, ${alpha / 255}), rgba(${rgb}, 0))`;
        el.animate(
            [
                {'--bx' : cx * 100 + '%', '--by' : cy * 100 + '%'} as any,
                {'--bx' : (cx + 0.08) * 100 + '%', '--by' : (cy - 0.06) * 100 + '%'} as any
            ],
            {duration : seconds * 1000, iterations : Infinity, direction : 'alternate', easing : 'ease-in-out'});
        return el;
    }

    private draw() {
        const media = this.mirror!;
        const ctx = this.canvas.getContext('2d')!;
        const W = this.canvas.width, H = this.canvas.height;
        const g = ctx.createLinearGradient(0, 0, W, H);
        g.addColorStop(0, '#1C2026');
        g.addColorStop(1, '#0B0D10');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, W, H);
        const v = media.video;
        const opacity = parseFloat(getComputedStyle(v).opacity) || 0;
        const box = media.el.getBoundingClientRect();
        if (v.readyState >= 2 && opacity > 0 && box.width > 0) {
            const va = v.videoWidth / v.videoHeight, ba = box.width / box.height;
            let sw = v.videoWidth, sh = v.videoHeight;
            if (va > ba)
                sw = sh * ba;
            else
                sh = sw / ba;
            const ca = W / H;
            let cw = sw, ch = sh;
            if (ba > ca)
                cw = ch * ca;
            else
                ch = cw / ca;
            ctx.globalAlpha = opacity;
            ctx.drawImage(v, (v.videoWidth - cw) / 2, (v.videoHeight - ch) / 2, cw, ch, 0, 0, W, H);
            ctx.globalAlpha = 1;
        }
        const r = ctx.createRadialGradient(W * 0.6, H * 0.35, 0, W * 0.6, H * 0.35, Math.max(W, H) * 0.9);
        r.addColorStop(0, 'rgba(0,0,0,0)');
        r.addColorStop(1, 'rgba(0,0,0,0.588)');
        ctx.fillStyle = r;
        ctx.fillRect(0, 0, W, H);
    }
}
