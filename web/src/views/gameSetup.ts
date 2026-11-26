import {on, rpc, isCanceled, messageOf, kindOf, type Call} from '../bridge';
import {h, icon, show, svg, tracked} from '../dom';
import {steamGlyph} from '../brand';
import {Progress, textBox, tooltip} from '../controls';
import {shell, type Snapshot, type View} from '../shell';
import {defaultButton, sideLayout} from './side';
import {outbreakDialog, type SteamSignIn} from './outbreak';

export type PromptKind = 'password'|'appCode'|'emailCode'|'mobileApproval';
export interface SteamProgress {
    stage: string;
    part: number;
    parts: number;
    fraction: number|null;
    detail: string|null;
}
interface BuildCheck {
    ok: boolean;
    title: string;
    reason: string;
    folder: string|null;
    kind?: string;
    stopTitle?: string;
    stopReason?: string;
}

export interface GameSetupView extends View {
    previewSteam(password?: boolean): void;
    previewPrompt(kind: PromptKind, email?: string|null, retry?: boolean): void;
    previewProgress(p: SteamProgress): void;
    ready: Promise<void>;
}

function steamButton(label: HTMLElement, style: Record<string, string> = {}) {
    const b = h('button.btn.steam', {type : 'button', style}) as HTMLButtonElement;
    b.append(svg(steamGlyph()), label);
    return b;
}

interface VerifyReport {
    folder: string;
    checked: number;
    problems: {path: string; issue : string}[];
    copies: number;
    fixed: number;
    fixedFiles: string[];
}

export function gameSetupView(fromSettings: boolean, startVerify = false): GameSetupView {
    const eyebrow = h('div.eyebrow.accent', tracked(fromSettings ? 'GAME FOLDER' : 'SETUP  ·  GAME FOLDER'));
    const pickTitle = fromSettings ? 'Find Para Bellum' : 'Install Para Bellum';
    const heading = h('div.title', {style : {fontSize : '34px', marginTop : '6px'}}, pickTitle);
    const game = textBox();
    const browseGame = h('button.btn.ghost', {type : 'button'}, icon(''), h('span', 'Browse')) as HTMLButtonElement;
    const checkIcon = icon('');
    const checkSpinner = new Progress();
    const checkTitle = h('div.strong', {style : {fontSize : '16px'}});
    const checkReason = h('div.body', {style : {fontSize : '14px', marginTop : '2px'}});
    const checkCard = h('div.glass.check-card.hidden', h('div.status', checkIcon, checkSpinner.el),
                        h('div', {style : {minWidth : '0', flex : '1'}}, checkTitle, checkReason));
    const continueButton = h('button.btn.primary.big', {type : 'button', style : {marginTop : '22px'}, disabled : true},
                             icon(''), h('span', 'Continue')) as HTMLButtonElement;
    const steamEntryText = fromSettings ? 'Sign in with Steam to download' : 'Download with Steam';
    const steamEntryLabel = h('span', steamEntryText);
    const showSteamButton = steamButton(steamEntryLabel);

    const verifyButton = h('button.link.hidden',
                           {type : 'button', style : {marginTop : '12px', fontSize : '14px', alignSelf : 'flex-start'}},
                           'Verify game files') as HTMLButtonElement;
    const verifyStage = h('div.strong', {style : {fontSize : '15px', flex : '1'}});
    const verifyPercent = h('div.strong', {style : {fontSize : '15px'}});
    const verifyBar = new Progress();
    verifyBar.el.style.marginTop = '8px';
    const verifyCancel = h('button.link', {type : 'button', style : {fontSize : '14px', marginLeft : '12px'}},
                           'Cancel') as HTMLButtonElement;
    const verifyDetail =
        h('div.body.nowrap', {style : {fontSize : '13px', color : 'var(--dim)', flex : '1', whiteSpace : 'nowrap'}});
    const verifyProgress = h('div.hidden', {style : {marginTop : '14px'}},
                             h('div', {style : {display : 'flex'}}, verifyStage, verifyPercent), verifyBar.el,
                             h('div', {style : {display : 'flex', marginTop : '8px', alignItems : 'flex-start'}},
                               verifyDetail, verifyCancel));
    const resultIcon = icon('');
    const resultTitle = h('div.strong', {style : {fontSize : '16px'}});
    const resultText = h('div.body', {style : {fontSize : '14px', marginTop : '2px'}});
    const resultList =
        h('div.body', {style : {fontSize : '13px', color : 'var(--dim)', marginTop : '6px', whiteSpace : 'pre-line'}});
    const repairButton = steamButton(h('span', 'Repair with Steam'), {marginTop : '14px', alignSelf : 'flex-start'});
    const verifyResult = h('div.glass.check-card.hidden', {style : {marginTop : '14px'}}, h('div.status', resultIcon),
                           h('div', {style : {minWidth : '0', flex : '1', display : 'flex', flexDirection : 'column'}},
                             resultTitle, resultText, resultList, repairButton));

    const folderBlock = h('div', {style : {display : 'flex', flexDirection : 'column'}},
                          h('div.eyebrow', {style : {marginBottom : '8px'}}, tracked('PARA BELLUM FOLDER')),
                          h('div.field-row', game.box, browseGame), checkCard, verifyButton, verifyProgress,
                          verifyResult, continueButton);
    const haveGame =
        h('button.link', {type : 'button', style : {marginTop : '18px', fontSize : '14px', alignSelf : 'flex-start'}},
          'I already have Para Bellum installed') as HTMLButtonElement;
    const showFolder = () => {
        show(folderBlock, true);
        show(haveGame, false);
    };
    const pickPanel =
        fromSettings
            ? h('div', {style : {marginTop : '26px', display : 'flex', flexDirection : 'column'}}, folderBlock,
                h('div.divider-row', {style : {margin : '30px 0 12px'}}, h('span.eyebrow', tracked('NO COPY?')),
                  h('div.rule')),
                showSteamButton)
            : h('div', {style : {marginTop : '26px', display : 'flex', flexDirection : 'column'}}, showSteamButton,
                h('div.body', {style : {fontSize : '14px', marginTop : '8px'}},
                  'Downloads Para Bellum with your own Steam account.'),
                haveGame, folderBlock);
    if (!fromSettings) {
        folderBlock.style.marginTop = '24px';
        show(folderBlock, false);
        haveGame.onclick = () => {
            showFolder();
            requestAnimationFrame(() => game.input.focus());
        };
    }

    const target = textBox();
    const browseTarget =
        h('button.btn.ghost', {type : 'button'}, icon(''), h('span', 'Browse')) as HTMLButtonElement;
    const targetText = h('div.body', {style : {fontSize : '14px', color : 'var(--dim)', marginTop : '8px'}});
    const startLabel = h('span', 'Sign in with Steam');
    const startButton = steamButton(startLabel, {marginTop : '20px'});
    const steamForm = h('div', {style : {display : 'flex', flexDirection : 'column'}},
                        h('div.eyebrow', {style : {marginBottom : '8px'}}, tracked('DOWNLOAD TO')),
                        h('div.field-row', target.box, browseTarget), targetText, startButton);

    const progressStage = h('div.strong', {style : {fontSize : '15px', flex : '1'}});
    const progressPercent = h('div.strong', {style : {fontSize : '15px'}});
    const bar = new Progress();
    bar.el.style.marginTop = '8px';
    const cancelButton = h('button.link', {type : 'button', style : {fontSize : '14px', marginLeft : '12px'}},
                           'Cancel') as HTMLButtonElement;
    const progressDetail =
        h('div.body.nowrap', {style : {fontSize : '13px', color : 'var(--dim)', flex : '1', whiteSpace : 'nowrap'}});
    const progressPanel = h('div.hidden', {style : {marginTop : '18px'}},
                            h('div', {style : {display : 'flex'}}, progressStage, progressPercent), bar.el,
                            h('div', {style : {display : 'flex', marginTop : '8px', alignItems : 'flex-start'}},
                              progressDetail, cancelButton));
    const source = h('button.link', {
                       type : 'button',
                       style : {fontSize : '13px', color : 'var(--dim)', display : 'inline-flex', alignItems : 'center'}
                   },
                     icon('', 'info-icon'), h('span', 'About this download')) as HTMLButtonElement;
    (source.querySelector('.info-icon') as HTMLElement).setAttribute('style', 'font-size:13px;margin-right:6px');
    tooltip(
        source,
        'Uses DepotDownloader by SteamRE (GPL-2.0), fetched unmodified from its official release. Your sign-in goes only to Steam; RWND doesn\'t store it. Click for the source and licence.');
    const licence = h('div', {style : {marginTop : '22px'}}, source);
    const steamPanel = h('div.hidden', {style : {marginTop : '26px'}}, steamForm, progressPanel, licence);
    const error = h('div.body.hidden', {style : {color : 'var(--bad)', marginTop : '14px'}});

    const content = h('div', eyebrow, heading, pickPanel, steamPanel, error);
    const layout = sideLayout(content, {scroll : true});
    show(layout.back, fromSettings);
    const steamModal = outbreakDialog(choice => download(choice), () => downloading?.cancel(), {
        eyebrow : 'GAME FOLDER',
        glyph : '\uE896',
        title : 'Download with Steam',
        text : 'Sign in with your own Steam account.'
    });
    layout.el.append(steamModal.el);

    let checkTimer = 0;
    let checking: Call<BuildCheck>|null = null;
    let downloading: Call<any>|null = null;
    let verifying: Call<VerifyReport>|null = null;
    let repairing = false;
    let autoVerified = false;
    let verifiedFolder: string|null = null;
    defaultButton(layout.el, () => steamModal.isOpen                         ? null
                                   : steamPanel.classList.contains('hidden') ? continueButton
                                                                             : startButton);

    const showError = (text: string, calm = false) => {
        error.textContent = text;
        error.style.color = calm ? 'var(--muted)' : 'var(--bad)';
        show(error, true);
    };

    function showCheck(ok: boolean|null, title: string, reason: string) {
        show(checkCard, true);
        show(checkSpinner.el, ok === null);
        show(checkIcon, ok !== null);
        checkIcon.textContent = ok === true ? '' : '';
        checkIcon.style.color = ok === true ? 'var(--good)' : 'var(--bad)';
        checkTitle.textContent = title;
        checkTitle.style.color = ok === false ? 'var(--bad)' : 'var(--text)';
        checkReason.textContent = reason;
    }

    async function check() {
        checking?.cancel();
        const folder = game.input.value.trim();
        showCheck(null, 'Checking the game...', 'Comparing RainbowSix.exe and the game data with Para Bellum.');
        const call = checking = rpc<BuildCheck>('gameSetup.verify', {folder});
        let result: BuildCheck;
        try {
            result = await call;
        } catch {
            return;
        }
        if (checking !== call) return;
        checking = null;
        if (result.kind === 'partial')
            showCheck(false, result.stopTitle ?? 'Download stopped', result.stopReason ?? 'Picks up where it stopped.');
        else
            showCheck(result.ok, result.title, result.reason);
        if (!result.ok) return;
        verifiedFolder = result.folder;
        continueButton.disabled = false;
        show(verifyButton, true);
        if (startVerify && !autoVerified) {
            autoVerified = true;
            runVerify();
        }
    }

    function gameChanged() {
        verifiedFolder = null;
        continueButton.disabled = true;
        show(verifyButton, false);
        show(verifyResult, false);
        checking?.cancel();
        checking = null;
        clearTimeout(checkTimer);
        if (game.input.value.trim().length === 0) {
            show(checkCard, false);
            return;
        }
        checkTimer = window.setTimeout(check, 350);
    }
    game.input.addEventListener('input', gameChanged);
    const setGame = (value: string) => {
        game.input.value = value;
        gameChanged();
    };

    browseGame.onclick = async () => {
        const chosen = await rpc<string|null>(
            'shell.browseFolder',
            {title : 'Choose your Para Bellum folder (it has RainbowSix.exe)', initial : game.input.value.trim()});
        if (!chosen) return;
        setGame(chosen);
        clearTimeout(checkTimer);
        check();
    };

    continueButton.onclick = async () => {
        if (!verifiedFolder) return;
        await rpc('gameSetup.finish', {folder : verifiedFolder});
        shell.finishGameSetup(fromSettings);
    };

    function showSteam(visible: boolean) {
        if (!visible) repairing = false;
        show(pickPanel, !visible);
        show(steamPanel, visible);
        show(layout.back, visible || fromSettings);
        show(error, false);
        heading.textContent = !visible ? pickTitle : repairing ? 'Repair with Steam' : 'Download with Steam';
        target.input.disabled = repairing;
        show(browseTarget, !repairing);
        startLabel.textContent = repairing ? 'Repair with Steam' : 'Sign in with Steam';
        if (visible) targetChanged();
    }

    function showVerifyProgress(p: {stage: string; fraction : number | null; detail : string}) {
        if (verifyCancel.disabled) return;
        verifyStage.textContent = p.stage;
        verifyBar.indeterminate = p.fraction === null;
        if (p.fraction !== null) verifyBar.value = p.fraction * 100;
        verifyPercent.textContent = p.fraction !== null ? Math.round(p.fraction * 100) + '%' : '';
        verifyDetail.textContent = p.detail;
    }

    function showVerifyResult(r: VerifyReport) {
        const bad = r.problems.length;
        show(verifyResult, true);
        resultIcon.textContent = bad ? '' : '';
        resultIcon.style.color = bad ? 'var(--bad)' : 'var(--good)';
        resultTitle.textContent = bad ? `${bad} game ${bad === 1 ? 'file is' : 'files are'} damaged or missing`
                                      : `All ${r.checked} game files are intact`;
        resultTitle.style.color = bad ? 'var(--bad)' : 'var(--text)';
        const copies = r.copies === 0 ? ''
                       : r.fixed > 0  ? `Fixed ${r.fixed} ${r.fixed === 1 ? 'file' : 'files'} in your account ${
                                            r.copies === 1 ? 'copy' : 'copies'}.`
                       : r.copies === 1 ? 'Your account copy is fine.'
                                        : `Your ${r.copies} account copies are fine.`;
        resultText.textContent =
            bad ? ('Repair downloads only these files again with your Steam account. ' + copies).trim()
                : copies || 'Nothing needed fixing.';
        resultList.textContent = r.problems.slice(0, 8).map(p => `${p.path}  (${p.issue})`).join('\n') +
                                 (bad > 8 ? `\n...and ${bad - 8} more` : '');
        show(resultList, bad > 0);
        show(repairButton, bad > 0);
        notifyIfAway('Game files checked', resultTitle.textContent ?? '');
    }

    async function runVerify() {
        if (!verifiedFolder || verifying || downloading || shell.busy) return;
        show(error, false);
        show(verifyResult, false);
        show(verifyButton, false);
        show(verifyProgress, true);
        verifyCancel.disabled = false;
        continueButton.disabled = true;
        browseGame.disabled = true;
        game.input.disabled = true;
        shell.busy = true;
        showVerifyProgress({stage : 'Starting', fraction : null, detail : ''});
        const call = verifying = rpc<VerifyReport>('gameFiles.verify', {folder : verifiedFolder});
        try {
            showVerifyResult(await call);
        } catch (e) {
            if (!isCanceled(e) || kindOf(e) === 'timeout')
                showError(messageOf(e));
            else
                showError('Verify stopped. Nothing was changed in the game folder.', true);
        } finally {
            verifying = null;
            shell.busy = false;
            show(verifyProgress, false);
            show(verifyButton, !!verifiedFolder);
            continueButton.disabled = !verifiedFolder;
            browseGame.disabled = false;
            game.input.disabled = false;
        }
    }
    verifyButton.onclick = () => runVerify();
    verifyCancel.onclick = () => {
        verifyCancel.disabled = true;
        verifyStage.textContent = 'Stopping...';
        verifying?.cancel();
    };
    repairButton.onclick = () => {
        if (!verifiedFolder) return;
        repairing = true;
        target.input.value = verifiedFolder;
        showSteam(true);
    };

    async function targetChanged() {
        if (repairing) {
            targetText.textContent = 'Checks ' + target.input.value + ' and downloads only the files that don\'t match.';
            return;
        }
        const result = await rpc<{target : string | null}>('gameSetup.resolveDownload', {folder : target.input.value});
        const typed = target.input.value.trim().replace(/[\\/]+$/, '').toLowerCase();
        targetText.textContent = !result.target                          ? 'Enter a valid folder.'
                                 : result.target.toLowerCase() === typed ? ''
                                                                         : 'Downloads to ' + result.target;
        show(targetText, targetText.textContent !== '');
    }
    target.input.addEventListener('input', targetChanged);
    showSteamButton.onclick = () => showSteam(true);
    startButton.onclick = () => {
        if (!downloading && !shell.busy)
            steamModal.open({title : repairing ? 'Repair with Steam' : 'Download with Steam'});
    };
    layout.back.onclick = () => {
        if (downloading || verifying) return;
        if (!steamPanel.classList.contains('hidden'))
            showSteam(false);
        else if (fromSettings)
            shell.showHome();
    };
    browseTarget.onclick = async () => {
        const chosen = await rpc<string|null>('shell.browseFolder', {
            title : 'Choose where to download Para Bellum',
            initial : target.input.value.trim(),
            parentFallback : true
        });
        if (chosen) {
            target.input.value = chosen;
            targetChanged();
        }
    };
    source.onclick = () => rpc('shell.openNotice');

    function showProgress(p: SteamProgress) {
        if (steamModal.isOpen && downloading &&
            (p.stage.startsWith('Signed in') || p.part > 0 || /^(Downloading|Checking)/.test(p.stage)))
            steamModal.started();
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
        const state = await rpc<{visible : boolean; active : boolean}>('shell.state');
        if (!state.active || !document.hasFocus()) shell.notify(title, text);
    }

    const offs = [
        on('steam.prompt',
           p => {
               if (!downloading) return;
               steamModal.ask(p.kind, p.email, p.retry);
               showProgress({stage : 'Waiting for you', part : 0, parts : 0, fraction : null, detail : null});
               notifyIfAway('Steam needs you', p.kind === 'mobileApproval' ? 'Approve the sign-in in the Steam app.'
                                                                           : 'Enter the code from Steam in RWND.');
           }),
        on('steam.progress', p => showProgress(p)),
        on('steam.qr',
           q => {
               if (downloading) {
                   steamModal.qr(q.rows);
                   notifyIfAway('Steam sign-in', 'Scan the QR code in RWND with the Steam Mobile app.');
               }
           }),
        on('gameFiles.progress', p => showVerifyProgress(p)),
    ];

    async function download(choice: SteamSignIn) {
        if (downloading || shell.busy) return;
        show(error, false);
        let started = false;
        const offStarted = on('steam.started', () => {
            started = true;
            shell.busy = true;
            show(steamForm, false);
            show(layout.back, false);
            show(progressPanel, true);
            cancelButton.disabled = false;
            showProgress({stage : 'Starting', part : 0, parts : 0, fraction : null, detail : null});
        });
        const wasRepair = repairing;
        const call = downloading = rpc('steam.download', {
            qr : !!choice.qr,
            user : choice.qr ? null : choice.user ?? '',
            password : choice.qr ? null : choice.password ?? null,
            folder : target.input.value,
            remember : choice.remember,
            repair : wasRepair
        });
        let resultTarget = '';
        try {
            const result = await call;
            resultTarget = result.target;
            if (result.result === 'alreadyGood') {
                showSteam(false);
                setGame(result.target);
                return;
            }
            if (result.result === 'cancelledEmpty') {
                showError('Cancelled. Nothing was downloaded.', true);
                return;
            }
            if (result.result === 'paused') {
                showError(
                    wasRepair
                        ? 'Repair stopped. Files Steam already replaced stay fixed. Sign in again to finish.'
                        : 'Download paused. Files already downloaded stay in the folder. Sign in again to resume.',
                    true);
                return;
            }
            showProgress({stage : 'Checking the download', part : 0, parts : 0, fraction : null, detail : null});
            showSteam(false);
            setGame(result.target);
            clearTimeout(checkTimer);
            await check();
            if (wasRepair && verifiedFolder) {
                setTimeout(() => runVerify(), 0);
                return;
            }
            if (verifiedFolder)
                shell.notify('Para Bellum downloaded',
                             'The game passed the build check. Click Continue to finish setup.');
        } catch (e) {
            const shown = !isCanceled(e) || kindOf(e) === 'timeout';
            if (shown && steamModal.isOpen && !started)
                steamModal.error(messageOf(e));
            else if (shown)
                showError(messageOf(e));
        } finally {
            offStarted();
            downloading = null;
            if (started && steamModal.isOpen) steamModal.close();
            if (started) {
                shell.busy = false;
                show(progressPanel, false);
                show(steamForm, true);
                show(layout.back, true);
                const snapshot = await rpc<Snapshot>('session.snapshot');
                const partial = !!resultTarget && snapshot.prefs.steamDownloadFolder !== null;
                startLabel.textContent = partial     ? 'Resume with Steam'
                                         : wasRepair ? 'Repair with Steam'
                                                     : 'Sign in with Steam';
                steamEntryLabel.textContent =
                    snapshot.prefs.steamDownloadFolder === null ? steamEntryText : 'Resume the Steam download';
            }
        }
    }
    cancelButton.onclick = () => {
        cancelButton.disabled = true;
        progressStage.textContent = 'Stopping...';
        downloading?.cancel();
    };

    const ready = (async () => {
        const snapshot = await rpc<Snapshot>('session.snapshot');
        const resume = snapshot.prefs.steamDownloadFolder;
        if (resume) steamEntryLabel.textContent = 'Resume the Steam download';
        target.input.value = resume ?? shell.info.defaultDownloadFolder;
        const initial = snapshot.prefs.gamePath.length > 0 ? snapshot.prefs.gamePath : resume ?? '';
        if (initial) {
            setGame(initial);
            if (!fromSettings && snapshot.prefs.gamePath.length > 0) showFolder();
        } else if (fromSettings)
            requestAnimationFrame(() => game.input.focus());
    })();

    const enterProgress = () => {
        showSteam(true);
        show(steamForm, false);
        show(progressPanel, true);
        cancelButton.disabled = false;
    };
    return {
        el : layout.el,
        ready,
        dispose() {
            clearTimeout(checkTimer);
            checking?.cancel();
            downloading?.cancel();
            verifying?.cancel();
            offs.forEach(off => off());
            steamModal.close();
            layout.dispose();
        },
        previewSteam(usePw = false) {
            showSteam(true);
            steamModal.open();
            if (usePw) steamModal.passwordMode();
        },
        previewPrompt(kind, email = null, retry = false) {
            enterProgress();
            steamModal.ask(kind, email, retry);
        },
        previewProgress(p) {
            enterProgress();
            showProgress(p);
        },
    };
}
