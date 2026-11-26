import {on, rpc} from './bridge';
import {h, icon, show} from './dom';
import {Progress} from './controls';

interface CrashReport {
    id: string;
    displayName: string;
    exitCode: string|null;
    hasDump: boolean;
    canSend: boolean;
    bundleAvailable: boolean;
}

interface Resolution {
    sent: boolean;
    reportId?: string;
}

function crashDialog(report: CrashReport, finish: (result: Resolution) => void): HTMLDialogElement {
    const canSend = report.canSend && report.bundleAvailable;
    const send = h('button.btn.primary', {type : 'button', autofocus : canSend}, 'Send report') as HTMLButtonElement;
    const cancel = h('button.btn.glass', {type : 'button', autofocus : !canSend}, canSend ? 'Cancel' : 'Close') as
                   HTMLButtonElement;
    show(send, canSend);
    const detail = h('p', {id : 'crash-description'},
                     !canSend         ? 'Any available crash files stay on this PC.'
                     : report.hasDump ? 'Send a crash report to help fix this.'
                                      : 'Send the crash log to help fix this.');
    const status = h('p.crash-status', {role : 'status', 'aria-live' : 'polite'});
    const progress = new Progress();
    progress.el.classList.add('crash-progress');
    progress.el.setAttribute('role', 'progressbar');
    show(progress.el, false);

    const reportDetails =
        report.hasDump && h('details.crash-details', h('summary', 'Report details'),
                            h('p', 'Crash log and a small snapshot of the game at the time of the crash.'));
    const marker = icon('', 'crash-mark');
    marker.setAttribute('aria-hidden', 'true');
    const dialog =
        h('dialog.crash-dialog', {'aria-labelledby' : 'crash-title', 'aria-describedby' : 'crash-description'},
          h('div.crash-heading', marker, h('div', h('h1', {id : 'crash-title'}, 'Game crashed'), detail)),
          report.displayName || report.exitCode
              ? h('div.crash-session', report.displayName ? h('span.crash-account', report.displayName) : null,
                  report.exitCode ? h('span.crash-exit', 'Exit code ', h('code', report.exitCode)) : null)
              : null,
          reportDetails, status, progress.el, h('div.crash-actions', cancel, send)) as HTMLDialogElement;

    let working = false;

    function setWorking(value: boolean, sending: boolean) {
        working = value;
        send.disabled = cancel.disabled = value;
        show(progress.el, value);
        progress.el.setAttribute('aria-label', sending ? 'Sending crash report' : 'Closing');
        if (sending) send.textContent = value ? 'Sending...' : 'Retry';
    }

    async function resolve(sending: boolean) {
        if (working) return;
        status.textContent = '';
        setWorking(true, sending);
        try {
            const result = await rpc<Resolution>('crash.resolve', {id : report.id, send : sending});
            if (sending && !result.sent) throw new Error('Upload was not accepted');
            finish(result);
        } catch {
            setWorking(false, sending);
            status.textContent =
                sending ? 'Upload failed. Retry or cancel.'
                        : 'RWND could not close the reporter. Your files are still on this PC. Please try again.';
            (sending ? send : cancel).focus();
        }
    }

    dialog.addEventListener('cancel', event => event.preventDefault());
    send.addEventListener('click', () => resolve(true));
    cancel.addEventListener('click', () => resolve(false));
    return dialog;
}

export function startCrashReporter(notify: (message: string) => void) {
    const pending = new Map<string, CrashReport>();
    const handled = new Set<string>();
    let active: HTMLDialogElement|null = null;

    function showNext() {
        if (active) return;
        const next = pending.values().next().value;
        if (!next) return;
        const dialog = crashDialog(next, result => {
            handled.add(next.id);
            pending.delete(next.id);
            dialog.close();
            dialog.remove();
            active = null;
            showNext();
            if (result.sent) notify(result.reportId ? 'Crash report sent · ' + result.reportId : 'Crash report sent.');
        });
        active = dialog;
        document.body.append(dialog);
        dialog.showModal();
    }

    function add(report: CrashReport, bringForward: boolean) {
        if (!report?.id || pending.has(report.id) || handled.has(report.id)) return;
        pending.set(report.id, report);
        showNext();
        if (bringForward) void rpc('shell.showFromTray').catch(() => {});
    }

    on('game.crashReport', report => add(report, true));
    void rpc<CrashReport[]>('crash.pending')
        .then(reports => {
            for (const [index, report] of reports.entries()) add(report, index === 0);
        })
        .catch(() => {});
}
