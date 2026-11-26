import {on, rpc, isCanceled, messageOf, kindOf, type Call} from '../bridge';
import {focusVisual, h, icon, show, tracked} from '../dom';
import {Progress, textBox, tooltip} from '../controls';
import {shell, type Snapshot, type View} from '../shell';
import {defaultButton, sideLayout} from './side';
import {SIZE_INFO, sizeGb, sizePage, parseSize} from './sizePage';
import {outbreakDialog, type SteamSignIn} from './outbreak';
import type {PromptKind, SteamProgress} from './gameSetup';

export type CheckKind = 'ok'|'missing'|'partial'|'other'|'fix';
export interface FolderCheck {
    ok: boolean;
    title: string;
    reason: string;
    folder: string|null;
    kind?: CheckKind;
    stopTitle?: string;
    stopReason?: string;
}
export const OUTBREAK_GB = '4.4';

export interface FirstRunView extends View {
    ready: Promise<void>;
    previewCheck(check: FolderCheck, folder: string): void;
    previewSize(modal?: 'none'|'choice'|'password'): void;
    previewProgress(outbreak: 'active'|'done', p: SteamProgress&{job?: string}): void;
    previewPrompt(kind: PromptKind): void;
    previewQr(): void;
}

export function firstRunView(): FirstRunView {
    const heading = h('div.title', {style : {fontSize : '34px', marginTop : '6px'}}, 'Game folder');
    const game = textBox();
    const browse = h('button.btn.ghost', {type : 'button'}, icon(''), h('span', 'Browse')) as HTMLButtonElement;
    const checkIcon = icon('');
    const checkSpinner = new Progress();
    const checkTitle = h('div.strong', {style : {fontSize : '16px'}});
    const checkReason = h('div.body', {style : {fontSize : '14px', marginTop : '2px'}});
    const checkCard = h('div.glass.check-card.hidden', h('div.status', checkIcon, checkSpinner.el),
                        h('div', {style : {minWidth : '0', flex : '1'}}, checkTitle, checkReason));
    const continueButton = h('button.btn.primary.big.hidden', {type : 'button', style : {marginTop : '22px'}},
                             icon(''), h('span', 'Continue')) as HTMLButtonElement;
    const folderBlock = h('div', {style : {display : 'flex', flexDirection : 'column', marginTop : '22px'}},
                          h('div.field-row', game.box, browse), checkCard, continueButton);

    const targetText = h('div.body', {style : {fontSize : '14px', color : 'var(--dim)', marginTop : '8px'}});
    const source = h('button.link', {
                       type : 'button',
                       style : {
                           fontSize : '13px',
                           color : 'var(--dim)',
                           display : 'inline-flex',
                           alignItems : 'center',
                           marginTop : '18px',
                           alignSelf : 'flex-start'
                       }
                   },
                     icon('', 'info-icon'), h('span', 'About this download')) as HTMLButtonElement;
    (source.querySelector('.info-icon') as HTMLElement).setAttribute('style', 'font-size:13px;margin-right:6px');
    tooltip(
        source,
        'Uses DepotDownloader by SteamRE (GPL-2.0), fetched unmodified from its official release. Your sign-in goes only to Steam; RWND doesn\'t store it. Click for the source and licence.');
    const nextButton = h('button.btn.primary.big', {type : 'button', style : {marginTop : '22px'}}, h('span', 'Next'),
                         icon('\uE72A')) as HTMLButtonElement;
    (nextButton.lastElementChild as HTMLElement).style.margin = '0 0 0 10px';
    const downloadBlock =
        h('div.hidden', {style : {display : 'flex', flexDirection : 'column'}}, targetText, nextButton);
    const downloadLabel = h('span', 'Download');
    const downloadButton = h('button.btn.primary.big', {type : 'button', style : {width : '300px'}}, icon('\uE896'),
                             downloadLabel) as HTMLButtonElement;
    const steamBlock =
        h('div', {style : {display : 'flex', flexDirection : 'column', marginTop : '26px'}}, downloadButton, source);
    const page = sizePage('SETUP', steamBlock, () => { gameStep.note.textContent = sizeGb(page.value) + ' GB'; });
    show(page.el, false);
    const steam = outbreakDialog(choice => download(choice), () => downloading?.cancel());
    const signInTexts = () => ({
        eyebrow : 'SETUP',
        glyph : '\uE896',
        title : (resumeHere ? 'Resume ' : 'Download ') + SIZE_INFO[page.value].name,
        text : `${sizeGb(page.value)} GB with your Steam account.`
    });
    const againTexts =
        {eyebrow : 'SETUP', glyph : '\uE896', title : 'Steam sign-in', text : 'Steam asks once more for the game.'};
    let resumeHere = false;

    const stepRow = (label: string, sizeText: string) => {
        const state = icon('');
        const note = h('div.body', {style : {fontSize : '14px'}}, sizeText);
        const el = h('div.step', h('div.step-icon', state),
                     h('div.strong', {style : {fontSize : '16px', flex : '1'}}, label), note);
        return {el, state, note};
    };
    const outbreakStep = stepRow('Outbreak', OUTBREAK_GB + ' GB');
    const gameStep = stepRow('Para Bellum', sizeGb(page.value) + ' GB');
    const steps = h('div.glass.steps', outbreakStep.el, gameStep.el);
    const setStep = (s: ReturnType<typeof stepRow>, mode: 'wait'|'active'|'done'|'later') => {
        s.state.textContent = mode === 'done' ? '' : mode === 'active' ? '' : '';
        s.state.style.color = mode === 'done' ? 'var(--good)' : mode === 'active' ? 'var(--accent)' : 'var(--dim)';
        s.el.classList.toggle('dim', mode === 'wait' || mode === 'later');
    };
    const progressStage = h('div.strong', {style : {fontSize : '15px', flex : '1'}});
    const progressPercent = h('div.strong', {style : {fontSize : '15px'}});
    const bar = new Progress();
    bar.el.style.marginTop = '8px';
    const cancelButton = h('button.link', {type : 'button', style : {fontSize : '14px', marginLeft : '12px'}},
                           'Cancel') as HTMLButtonElement;
    const progressDetail =
        h('div.body.nowrap', {style : {fontSize : '13px', color : 'var(--dim)', flex : '1', whiteSpace : 'nowrap'}});
    const progressBlock = h('div.hidden', {style : {marginTop : '22px'}}, steps,
                            h('div', {style : {marginTop : '18px'}},
                              h('div', {style : {display : 'flex'}}, progressStage, progressPercent), bar.el,
                              h('div', {style : {display : 'flex', marginTop : '8px', alignItems : 'flex-start'}},
                                progressDetail, cancelButton)));

    const error = h('div.body.hidden', {style : {color : 'var(--bad)', marginTop : '14px'}});
    const content =
        h('div', h('div.eyebrow.accent', tracked('SETUP')), heading, folderBlock, downloadBlock, progressBlock, error);
    const layout = sideLayout(content, {scroll : true});
    show(layout.back, false);
    const root = h('div', {style : {position : 'absolute', inset : '0'}}, layout.el, page.el, steam.el);
    const showPage = (visible: boolean) => {
        show(page.el, visible);
        show(layout.el, !visible);
        if (visible) focusVisual(downloadButton);
    };
    nextButton.onclick = () => {
        targetChanged();
        showPage(true);
    };
    page.back.onclick = () => {
        if (!downloading) showPage(false);
    };

    let snapshot: Snapshot|null = null;
    let checkTimer = 0;
    let checking: Call<FolderCheck>|null = null;
    let downloading: Call<any>|null = null;
    let lastCheck: FolderCheck|null = null;
    let autoContinue = false;
    let outbreakPlanned = false, outbreakSeen = false;
    let startedIn: string|null = null;
    let keepError = false;
    defaultButton(root, () => steam.isOpen                                   ? null
                              : !page.el.classList.contains('hidden')        ? downloadButton
                              : !continueButton.classList.contains('hidden') ? continueButton
                                                                             : nextButton);

    const showError = (text: string, calm = false) => {
        error.textContent = text;
        error.style.color = calm ? 'var(--muted)' : 'var(--bad)';
        show(error, true);
    };

    function showCheck(state: 'busy'|'ok'|'bad'|'download', title: string, reason: string) {
        show(checkCard, true);
        show(checkSpinner.el, state === 'busy');
        show(checkIcon, state !== 'busy');
        checkIcon.textContent = state === 'ok' ? '' : state === 'download' ? '' : '';
        checkIcon.style.color = state === 'ok' ? 'var(--good)' : state === 'download' ? 'var(--accent)' : 'var(--bad)';
        checkTitle.textContent = title;
        checkTitle.style.color = state === 'bad' ? 'var(--bad)' : 'var(--text)';
        checkReason.textContent = reason;
    }

    function apply(result: FolderCheck) {
        lastCheck = result;
        const kind: CheckKind = result.ok ? 'ok' : result.kind ?? 'fix';
        if (!keepError || kind === 'partial' || kind === 'ok') show(error, false);
        keepError = false;
        show(continueButton, kind === 'ok');
        show(downloadBlock, kind !== 'ok');
        if (kind === 'ok')
            showCheck('ok', result.title, 'Ready to play.');
        else if (kind === 'missing')
            showCheck('download', 'Not installed yet', 'Downloads with your Steam account.');
        else if (kind === 'partial')
            showCheck('download', result.stopTitle ?? 'Download stopped',
                      result.stopReason ?? 'Picks up where it stopped.');
        else
            showCheck('bad', result.title, result.reason);
        resumeHere = kind === 'partial';
        downloadLabel.textContent = resumeHere ? 'Resume' : 'Download';
        targetChanged();
        if (kind === 'ok' && autoContinue) {
            autoContinue = false;
            finish(result.folder!);
        }
    }

    async function check() {
        checking?.cancel();
        const folder = game.input.value.trim();
        show(continueButton, false);
        if (!folder) {
            show(checkCard, false);
            apply({ok : false, title : '', reason : '', folder : null, kind : 'missing'});
            show(checkCard, false);
            return;
        }
        showCheck('busy', 'Checking...', '');
        const call = checking = rpc<FolderCheck>('gameSetup.verify', {folder});
        let result: FolderCheck;
        try {
            result = await call;
        } catch {
            return;
        }
        if (checking !== call) return;
        checking = null;
        apply(result);
    }
    const queueCheck = () => {
        clearTimeout(checkTimer);
        checkTimer = window.setTimeout(check, 350);
    };
    game.input.addEventListener('input', () => {
        autoContinue = false;
        queueCheck();
    });
    const setGame = (value: string) => {
        game.input.value = value;
        clearTimeout(checkTimer);
        return check();
    };

    browse.onclick = async () => {
        const chosen = await rpc<string|null>(
            'shell.browseFolder',
            {title : 'Choose a folder for Para Bellum', initial : game.input.value.trim(), parentFallback : true});
        if (chosen) {
            autoContinue = false;
            setGame(chosen);
        }
    };

    const downloadFolder = () => lastCheck?.kind === 'other' || !game.input.value.trim()
                                     ? shell.info.defaultDownloadFolder
                                     : game.input.value.trim();
    async function targetChanged() {
        const folder = downloadFolder();
        const result =
            await rpc<{target : string | null}>('gameSetup.resolveDownload', {folder}).catch(() => ({target : folder}));
        const typed = game.input.value.trim().replace(/[\\/]+$/, '').toLowerCase();
        targetText.textContent = !result.target                          ? 'Choose a valid folder.'
                                 : result.target.toLowerCase() === typed ? ''
                                                                         : 'Downloads to ' + result.target;
        show(targetText, targetText.textContent !== '');
        gameStep.note.textContent = sizeGb(page.value) + ' GB';
    }

    async function finish(folder: string) {
        await rpc('gameSetup.finish', {folder});
        shell.finishGameSetup(false);
    }
    continueButton.onclick = () => {
        if (lastCheck?.ok && lastCheck.folder) finish(lastCheck.folder);
    };

    downloadButton.onclick = () => {
        if (!downloading && !shell.busy) steam.open(signInTexts());
    };
    source.onclick = () => rpc('shell.openNotice');

    function enterProgress() {
        show(folderBlock, false);
        show(downloadBlock, false);
        show(error, false);
        show(progressBlock, true);
        heading.textContent = 'Downloading';
        outbreakPlanned = !!snapshot?.outbreak?.live && !snapshot.outbreak.ready;
        outbreakSeen = false;
        show(outbreakStep.el, outbreakPlanned);
        setStep(outbreakStep, outbreakPlanned ? 'active' : 'wait');
        setStep(gameStep, outbreakPlanned ? 'wait' : 'active');
        cancelButton.disabled = false;
    }
    function leaveProgress(recheck: boolean) {
        show(progressBlock, false);
        show(folderBlock, true);
        heading.textContent = 'Game folder';
        keepError = !error.classList.contains('hidden');
        if (recheck && startedIn) {
            game.input.value = startedIn;
            clearTimeout(checkTimer);
            check();
        } else if (lastCheck)
            apply(lastCheck);
    }

    function showProgress(p: SteamProgress&{job?: string}) {
        const outbreakPart = p.job === 'outbreak';
        if (outbreakPart) outbreakSeen = true;
        if (outbreakPlanned && !outbreakPart && (p.part > 0 || p.stage === 'Connecting to Steam' && outbreakSeen)) {
            setStep(outbreakStep, outbreakSeen ? 'done' : 'later');
            if (!outbreakSeen) outbreakStep.note.textContent = 'Later';
            setStep(gameStep, 'active');
        }
        if (steam.isOpen && downloading &&
            (p.stage.startsWith('Signed in') || p.part > 0 || /^(Downloading|Checking|Converting)/.test(p.stage)))
            steam.started();
        if (cancelButton.disabled) return;
        progressStage.textContent = p.part > 0 ? `${p.stage}  ·  part ${p.part} of ${p.parts}` : p.stage;
        bar.indeterminate = p.fraction === null;
        if (p.fraction !== null) bar.value = p.fraction * 100;
        progressPercent.textContent = p.fraction === null ? ''
                                      : p.fraction < 0.1  ? (Math.floor(p.fraction * 1000) / 10).toFixed(1) + '%'
                                                          : Math.round(p.fraction * 100) + '%';
        progressDetail.textContent = p.detail ?? '';
    }

    async function notifyIfAway(title: string, text: string) {
        const state = await rpc<{visible : boolean; active : boolean}>('shell.state')
                          .catch(() => ({visible : true, active : true}));
        if (!state.active || !document.hasFocus()) shell.notify(title, text);
    }

    const offs = [
        on('steam.prompt',
           p => {
               if (!downloading) return;
               steam.ask(p.kind, p.email, p.retry, againTexts);
               showProgress({stage : 'Waiting for you', part : 0, parts : 0, fraction : null, detail : null});
               notifyIfAway('Steam needs you', p.kind === 'mobileApproval' ? 'Approve the sign-in in the Steam app.'
                                                                           : 'Enter the code from Steam in RWND.');
           }),
        on('steam.progress', p => showProgress(p)),
        on('steam.qr',
           q => {
               if (!downloading) return;
               steam.qr(q.rows, againTexts);
               notifyIfAway('Steam sign-in', 'Scan the QR code in RWND with the Steam Mobile app.');
           }),
    ];

    async function download(choice: SteamSignIn) {
        if (downloading || shell.busy) return;
        show(error, false);
        let started = false, checked = false, finishedFolder: string|null = null;
        const offStarted = on('steam.started', s => {
            started = true;
            startedIn = s?.target ?? null;
            shell.busy = true;
            showPage(false);
            enterProgress();
            showProgress({stage : 'Starting', part : 0, parts : 0, fraction : null, detail : null});
        });
        const call = downloading = rpc('steam.download', {
            qr : !!choice.qr,
            user : choice.qr ? null : choice.user ?? '',
            password : choice.qr ? null : choice.password ?? null,
            folder : downloadFolder(),
            remember : choice.remember,
            repair : false,
            size : page.value
        });
        try {
            const result = await call;
            if (result.result === 'cancelledEmpty') {
                showError('Cancelled. Nothing was downloaded.', true);
                return;
            }
            if (result.result === 'paused') {
                showError('Paused. Sign in again to resume.', true);
                return;
            }
            showProgress({stage : 'Checking the game', part : 0, parts : 0, fraction : null, detail : null});
            const verified = await rpc<FolderCheck>('gameSetup.verify', {folder : result.target});
            if (verified.ok && verified.folder)
                finishedFolder = verified.folder;
            else {
                game.input.value = result.target;
                lastCheck = verified;
                checked = true;
            }
        } catch (e) {
            const shown = !isCanceled(e) || kindOf(e) === 'timeout';
            if (shown && steam.isOpen && !started)
                steam.error(messageOf(e));
            else if (shown)
                showError(messageOf(e));
        } finally {
            offStarted();
            downloading = null;
            if (started && steam.isOpen) steam.close();
            if (started) shell.busy = false;
            if (started && !finishedFolder) leaveProgress(!checked);
        }
        if (finishedFolder) {
            notifyIfAway('Para Bellum is ready', 'Press Play.');
            await finish(finishedFolder);
        }
    }
    cancelButton.onclick = () => {
        cancelButton.disabled = true;
        progressStage.textContent = 'Stopping...';
        downloading?.cancel();
    };

    const ready = (async () => {
        snapshot = await rpc<Snapshot>('session.snapshot');
        page.value = snapshot.prefs.steamDownloadFolder ? parseSize(snapshot.prefs.downloadSize) : 'medium';
        const saved = snapshot.prefs.gamePath;
        if (saved) {
            autoContinue = true;
            await setGame(saved);
            return;
        }
        let folder = snapshot.prefs.steamDownloadFolder ?? '';
        if (!folder) {
            showCheck('busy', 'Looking for Para Bellum...', '');
            folder = (await rpc<{folder : string | null}>('gameSetup.detect').catch(() => null))?.folder ??
                     shell.info.defaultDownloadFolder;
        }
        await setGame(folder);
    })();

    return {
        el : root,
        ready,
        dispose() {
            clearTimeout(checkTimer);
            checking?.cancel();
            downloading?.cancel();
            offs.forEach(off => off());
            layout.dispose();
            page.dispose();
        },
        previewCheck(c, folder) {
            checking?.cancel();
            checking = null;
            clearTimeout(checkTimer);
            game.input.value = folder;
            apply(c);
        },
        previewSize(modal = 'none') {
            targetChanged();
            showPage(true);
            if (modal !== 'none') steam.open(signInTexts());
            if (modal === 'password') steam.passwordMode();
        },
        previewProgress(outbreak, p) {
            snapshot = {
                ...snapshot!,
                outbreak : {live : true, ready : false, update : false, steam : false, installing : false}
            };
            enterProgress();
            if (outbreak === 'done') {
                outbreakSeen = true;
            }
            showProgress(p);
        },
        previewPrompt(kind) {
            enterProgress();
            outbreakSeen = true;
            showProgress({stage : 'Connecting to Steam', part : 0, parts : 0, fraction : null, detail : null});
            downloading = {cancel() {}} as Call<any>;
            steam.ask(kind, null, false, againTexts);
        },
        previewQr() {
            enterProgress();
            outbreakSeen = true;
            showProgress({stage : 'Connecting to Steam', part : 0, parts : 0, fraction : null, detail : null});
            const n = 29, rows: string[] = [];
            for (let y = 0; y < n; y++) {
                let r = '';
                for (let x = 0; x < n; x++) {
                    const finder = (a: number, b: number) =>
                        a < 7 && b < 7 &&
                        (a === 0 || a === 6 || b === 0 || b === 6 || (a > 1 && a < 5 && b > 1 && b < 5));
                    const f = finder(x, y) || finder(n - 1 - x, y) || finder(x, n - 1 - y);
                    const inFinder = (x < 8 && y < 8) || (x > n - 9 && y < 8) || (x < 8 && y > n - 9);
                    r += inFinder ? (f ? '1' : '0') : ((x * 7 + y * 13 + x * y) % 5 < 2 ? '1' : '0');
                }
                rows.push(r);
            }
            downloading = {cancel() {}} as Call<any>;
            steam.qr(rows, againTexts);
        },
    };
}
