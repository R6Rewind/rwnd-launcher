import {on, rpc, kindOf, messageOf, RpcError} from './bridge';
import {$, show, svg} from './dom';
import {rBlock} from './brand';
import {Progress} from './controls';
import {setVisible} from './backdrop';

export interface AppInfo {
    mode: 'setup'|'launcher'|'preview';
    version: string;
    releaseBuild: boolean;
    installed: boolean;
    installDirectory: string;
    defaultInstallDirectory: string;
    registeredDirectory: string|null;
    defaultDownloadFolder: string;
    media: string[];
    maximized: boolean;
    websiteUrl: string|null;
}
export interface Release {
    version: string;
    url: string;
    size: number;
    notes: string|null;
}
export interface View {
    el: HTMLElement;
    dispose?(): void;
}

export interface Account {
    playerId: string;
    displayName: string;
    avatarUrl: string|null;
    hasRefresh: boolean;
    canSignInSilently: boolean;
    inGame: boolean;
    gameStartTicks: number|null;
}
export interface Snapshot {
    accounts: Account[];
    selected: string|null;
    runningCount: number;
    busy: boolean;
    wasCorrupt: boolean;
    needsGameSetup: boolean;
    accountLimit: number;
    instances: string;
    nowTicks: number;
    prefs: {
        gamePath: string; friendNotifications : boolean;
        messageNotifications?: boolean; keepInTray : boolean; updateNotifications : boolean;
        steamDownloadFolder : string | null;
        downloadSize?: 'full' | 'medium' | 'small';
        mouseMultiplier?: number | null;
        fpsLimit?: number | null
    };
    outbreak
        ?: {live: boolean; ready : boolean; update : boolean; steam : boolean; installing : boolean; version?: string};
}

export const isUnreachable = (e: unknown) =>
    e instanceof RpcError && (e.kind === 'timeout' || (e.kind === 'http' && e.status === 0));
const UnreachableAfterMs = 15_000;

type ViewFactory = {
    login(adding: boolean, reauthenticate?: string): View; home(settings: boolean) : View;
    gameSetup(fromSettings: boolean, verify?: boolean) : View;
    firstRun() : View;
    size() : View;
    setup() : View
};

class Shell {
    info!: AppInfo;
    views!: ViewFactory;
    current?: View;
    pendingUpdate: Release|null = null;
    private updating = false;
    private lastUpdateCheck = 0;
    private toastTimer = 0;
    private busyValue = false;
    private overlayProgress = new Progress();
    readonly seenRequests = new Map<string, Set<string>>();
    home?: {
        playFromTray(): void; signOutFromTray() : void; showFriends() : void; openSettings() : void;
        showMessages ? (name?: string) : void
    };

    get busy() { return this.busyValue; }
    set busy(value: boolean) {
        this.busyValue = value;
        rpc('shell.setBusy', {busy : value});
    }

    init(info: AppInfo, views: ViewFactory) {
        this.info = info;
        this.views = views;
        $('#overlay-mark').append(svg(rBlock()));
        $('#overlay-progress').replaceWith(this.overlayProgress.el);
        this.overlayProgress.el.id = 'overlay-progress';
        $('#minimize').onclick = () => rpc('window.minimize');
        $('#maximize').onclick = () => rpc('window.toggleMaximize');
        $('#close').onclick = () => rpc('window.close');
        $('#download-update').onclick = () => this.update();
        $('#update-later').onclick = () => this.hideOverlay();
        for (const edge of document.querySelectorAll<HTMLElement>('.edge'))
            edge.addEventListener('mousedown', e => {
                if (e.button === 0) {
                    e.preventDefault();
                    rpc('window.startResize', {edge : edge.dataset.edge});
                }
            });
        this.setMaximized(info.maximized);
        on('window.state', s => this.setMaximized(s.maximized));
        on('window.visible', s => setVisible(s.visible));
        on('toast', t => this.showToast(t.text));
        on('tray.play', () => this.home?.playFromTray());
        on('tray.signOut', () => this.home?.signOutFromTray());
        on('tray.friends', () => this.home?.showFriends());
        on('tray.messages', (m: {with?: string}|null) => this.home?.showMessages?.(m?.with));
        on('shell.shown', () => {
            if (this.pendingUpdate)
                this.showUpdatePrompt();
            else if (Date.now() - this.lastUpdateCheck > 10 * 60_000)
                this.checkForUpdate();
        });
        document.addEventListener('contextmenu', e => {
            if (!(e.target as HTMLElement).closest('input')) e.preventDefault();
        });
        document.addEventListener('keydown', e => {
            if ((e.ctrlKey && [ 'p', 'f', 'g', 'j', 'u', 's', 'o', 'r' ].includes(e.key.toLowerCase())) ||
                e.key === 'F5' || e.key === 'F7')
                e.preventDefault();
        });
    }

    private setMaximized(maximized: boolean) {
        $('#root').classList.toggle('maximized', maximized);
        $('#maximize').textContent = maximized ? '' : '';
        $('#maximize').title = maximized ? 'Restore' : 'Maximize';
    }

    show(view: View) {
        this.current?.dispose?.();
        const host = $('#host');
        host.replaceChildren(view.el);
        this.current = view;
    }

    async start() {
        if (this.info.mode === 'setup') {
            this.show(this.views.setup());
            return;
        }
        if (this.info.installed && await this.update()) return;
        const snapshot = await rpc<Snapshot>('session.snapshot');
        if (snapshot.wasCorrupt) this.showToast('Saved login data could not be read and was removed. Sign in again.');
        if (snapshot.accounts.length === 0)
            this.showLogin(false);
        else if (snapshot.needsGameSetup)
            this.showGameSetup(false);
        else
            this.showHome();
        setInterval(() => this.checkForUpdate(), 10 * 60_000);
    }

    showGameSetup(fromSettings: boolean, verify = false) {
        this.home = undefined;
        this.show(fromSettings ? this.views.gameSetup(fromSettings, verify) : this.views.firstRun());
        this.updateTray(null, 'Finish setup to play', false);
    }

    async finishGameSetup(fromSettings: boolean) {
        if (fromSettings) {
            this.showHome();
            this.showToast('Game folder saved.');
            return;
        }
        const snapshot = await rpc<Snapshot>('session.snapshot');
        if (snapshot.accounts.length > 0)
            this.showHome();
        else
            this.showLogin(false);
    }

    async afterSignIn() {
        const snapshot = await rpc<Snapshot>('session.snapshot').catch(() => null);
        if (snapshot?.needsGameSetup)
            this.showGameSetup(false);
        else
            this.showHome();
    }

    showLogin(adding: boolean, reauthenticate?: string) {
        this.home = undefined;
        this.show(this.views.login(adding, reauthenticate));
        this.updateTray(null, 'Sign in to play RWND', false);
    }

    showSize() {
        this.home = undefined;
        this.show(this.views.size());
    }

    showHome(settings = false) { this.show(this.views.home(settings)); }

    updateTray(account: string|null, playLabel: string, canPlay: boolean) {
        rpc('shell.updateTray', {account, playLabel, canPlay});
    }
    notify(title: string, text: string) { rpc('shell.notify', {title, text}); }

    async checkForUpdate(): Promise<Release|null> {
        if (!this.info.installed || this.updating || this.info.mode !== 'launcher') return this.pendingUpdate;
        this.lastUpdateCheck = Date.now();
        try {
            const release = await rpc<Release|null>('update.check');
            if (release && this.pendingUpdate?.version !== release.version) await this.offerUpdate(release, true);
        } catch {
        }
        return this.pendingUpdate;
    }

    async offerUpdate(release: Release, notify: boolean) {
        this.pendingUpdate = release;
        if (notify) {
            const snapshot = await rpc<Snapshot>('session.snapshot').catch(() => null);
            if (snapshot?.prefs.updateNotifications)
                rpc('shell.trayNotify', {
                    title : 'RWND update available',
                    text : `Version ${release.version} is ready. Open RWND to update.`
                });
        }
        this.showUpdatePrompt();
    }

    async showUpdatePrompt() {
        const release = this.pendingUpdate;
        if (!release || this.updating) return;
        if (this.busy) {
            setTimeout(() => this.showUpdatePrompt(), 30_000);
            return;
        }
        this.showOverlay('Update available', `RWND Parabellum v${release.version} is ready.`);
        show(this.overlayProgress.el, false);
        show($('#update-actions'), true);
        const snapshot = await rpc<Snapshot>('session.snapshot').catch(() => null);
        const gameRunning = (snapshot?.runningCount ?? 0) > 0;
        ($('#download-update') as HTMLButtonElement).disabled = gameRunning;
        $('#update-hint').textContent = gameRunning ? 'Close Parabellum first. Updating restarts the launcher.'
                                                    : "Takes about a minute. The launcher restarts when it's done.";
    }

    previewUpdatePrompt(release: Release) {
        this.showOverlay('Update available', `RWND Parabellum v${release.version} is ready.`);
        show(this.overlayProgress.el, false);
        show($('#update-actions'), true);
        $('#update-hint').textContent = "Takes about a minute. The launcher restarts when it's done.";
    }

    async update(): Promise<boolean> {
        for (;;) {
            const result = await this.checkAndInstall();
            if (result !== 'unreachable') return result === 'restarting';
            if (!await this.showUnreachable()) return false;
        }
    }

    private async checkAndInstall(): Promise<'restarting'|'continue'|'unreachable'> {
        this.updating = true;
        this.lastUpdateCheck = Date.now();
        show($('#update-actions'), false);
        show($('#unreachable-actions'), false);
        show(this.overlayProgress.el, true);
        this.overlayProgress.indeterminate = true;
        this.showOverlay('Checking for updates', 'RWND Parabellum v' + this.info.version);
        let downloading = false, stalled = false;
        const offDownloading = on('update.downloading', (r: Release) => {
            downloading = true;
            this.showOverlay('Updating to v' + r.version, 'Downloading...');
            this.overlayProgress.indeterminate = false;
        });
        const offProgress = on('update.progress', p => {
            this.overlayProgress.value = p.fraction * 100;
            $('#overlay-text').textContent =
                p.restarting ? 'Restarting...' : `Downloading... ${Math.round(p.fraction * 100)}%`;
        });
        let watchdog = 0;
        try {
            const stayInTray = !(await rpc('shell.state')).visible;
            const call = rpc<{restarting : boolean}>('update.install', {stayInTray});
            watchdog = window.setTimeout(() => {
                if (!downloading) {
                    stalled = true;
                    call.cancel();
                }
            }, UnreachableAfterMs);
            const state = await call;
            if (!state.restarting) this.pendingUpdate = null;
            return state.restarting ? 'restarting' : 'continue';
        } catch (e) {
            if (stalled || isUnreachable(e)) return 'unreachable';
            if (kindOf(e) === 'canceled') return 'restarting';
            this.showToast("Couldn't update the launcher (" + messageOf(e).replace(/\.+$/, '') +
                           '). Continuing with v' + this.info.version + '.');
            return 'continue';
        } finally {
            clearTimeout(watchdog);
            offDownloading();
            offProgress();
            show($('#overlay'), false);
            this.overlayProgress.indeterminate = true;
            this.updating = false;
        }
    }

    private showUnreachable(): Promise<boolean> {
        this.showOverlay("Can't reach RWND", 'Check your connection.');
        show(this.overlayProgress.el, false);
        show($('#update-actions'), false);
        show($('#unreachable-actions'), true);
        return new Promise(resolve => {
            $('#retry-connect').onclick = () => {
                show($('#unreachable-actions'), false);
                resolve(true);
            };
            $('#continue-offline').onclick = () => {
                show($('#unreachable-actions'), false);
                this.hideOverlay();
                resolve(false);
            };
        });
    }

    showOverlay(title: string, text: string) {
        $('#overlay-title').textContent = title;
        $('#overlay-text').textContent = text;
        show($('#overlay'), true);
    }
    hideOverlay() { show($('#overlay'), false); }

    showToast(message: string, action?: {label: string; run : () => void}) {
        $('#toast-text').textContent = message;
        const button = $('#toast-action') as HTMLButtonElement;
        button.textContent = action?.label ?? '';
        button.onclick = action ? () => {
            show($('#toast'), false);
            action.run();
        } : null;
        show(button, !!action);
        show($('#toast'), true);
        clearTimeout(this.toastTimer);
        this.toastTimer = window.setTimeout(() => show($('#toast'), false), action ? 12000 : 6000);
    }
}

export const shell = new Shell();
