import {on, rpc, kindOf, messageOf} from '../bridge';
import {h, icon, show} from '../dom';
import {avatar, tooltip} from '../controls';
import {shell} from '../shell';

export interface ChatLast {
    text: string;
    mine: boolean;
    at: string;
}
export interface ChatFriend {
    name: string;
    avatarUrl: string|null;
    online: boolean;
    game: boolean;
    unread: number;
    last: ChatLast|null;
}
export interface ChatState {
    available: boolean;
    connected: boolean;
    friends: ChatFriend[];
}
export interface ChatMessage {
    with: string;
    id: number;
    mine: boolean;
    text: string;
    at: string;
}
export interface ChatHistory {
    items: ChatMessage[];
    more: boolean;
}

export const MESSAGE_LIMIT = 500;

export interface MessagesView {
    page: HTMLElement;
    badge: HTMLElement;
    open(name: string): void;
    decorate(row: HTMLElement, name: string): void;
    unread(name: string): number;
    shown(visible: boolean): void;
    setAccount(id: string|null): void;
    showSample(state: ChatState, open: string, history: ChatMessage[], draft?: string): void;
    dispose(): void;
}

const time = (iso: string) => {
    const d = new Date(iso);
    return isNaN(d.getTime()) ? '' : d.toLocaleTimeString('en-GB', {hour : '2-digit', minute : '2-digit'});
};
const day = (iso: string) => {
    const d = new Date(iso), now = new Date();
    if (isNaN(d.getTime())) return '';
    const days =
        Math.floor((new Date(now.toDateString()).getTime() - new Date(d.toDateString()).getTime()) / 86_400_000);
    return days === 0   ? 'Today'
           : days === 1 ? 'Yesterday'
                        : d.toLocaleDateString('en-GB', {day : '2-digit', month : 'short'});
};

export function messagesView(onChanged: () => void): MessagesView {
    let account: string|null = null;
    let state: ChatState = {available : false, connected : false, friends : []};
    let current: string|null = null;
    let history: ChatMessage[] = [];
    let more = false;
    let visible = false;
    let sample = false;

    const badge = h('span.nav-badge.hidden');
    const list = h('div.chat-list.scroll');
    const empty = h('div.chat-empty', icon(''), h('div.body', 'Pick a friend'));
    const headAvatar = h('div');
    const headName = h('div.strong.nowrap');
    const headState = h('span.chat-tag');
    const blockButton = h('button.chrome-button', {type : 'button'}, '') as HTMLButtonElement;
    tooltip(blockButton, 'Block');
    const head = h('div.chat-head', headAvatar, h('div.who', headName, headState), blockButton);
    const olderButton = h('button.link.chat-older', {type : 'button'}, 'Older') as HTMLButtonElement;
    const stream = h('div.chat-stream');
    const scroller = h('div.chat-scroll.scroll', olderButton, stream);
    const input = h('input', {
                      type : 'text',
                      placeholder : 'Message',
                      spellcheck : true,
                      autocomplete : 'off',
                      maxLength : MESSAGE_LIMIT
                  }) as HTMLInputElement;
    const sendButton = h('button.btn.primary.chat-send', {type : 'button'}, icon('')) as HTMLButtonElement;
    tooltip(sendButton, 'Send');
    const composer = h('div.chat-composer', input, sendButton);
    const offline = h('div.chat-off.body');
    const conversation = h('div.chat-conv.glass', head, scroller, composer);
    const page = h('div.page.chat-page.hidden', h('div.chat-side.glass', list),
                   h('div.chat-main', empty, conversation, offline));

    const friend = (name: string|null) =>
        state.friends.find(f => f.name.toLowerCase() === (name ?? '').toLowerCase()) ?? null;
    const totalUnread = () => state.friends.reduce((n, f) => n + f.unread, 0);

    function renderBadge() {
        const n = totalUnread();
        badge.textContent = n > 99 ? '99+' : String(n);
        show(badge, n > 0);
    }

    function renderList() {
        list.replaceChildren();
        const sorted = [...state.friends ].sort((a, b) => (Number(b.online) - Number(a.online)) ||
                                                          ((b.last?.at ?? '') < (a.last?.at ?? '')   ? -1
                                                           : (b.last?.at ?? '') > (a.last?.at ?? '') ? 1
                                                                                                     : 0) ||
                                                          a.name.localeCompare(b.name));
        let lastOnline: boolean|null = null;
        for (const f of sorted) {
            if (f.online !== lastOnline) {
                const count = sorted.filter(x => x.online === f.online).length;
                list.append(h('div.section-head' + (lastOnline === null ? '.first' : ''),
                              h('div.body', f.online ? 'Online' : 'Offline'),
                              h('div.badge', h('span.eyebrow', String(count)))));
                lastOnline = f.online;
            }
            const face = avatar(34, f.name, f.avatarUrl, f.online ? 'var(--good)' : null);
            if (!f.online) face.style.opacity = '0.55';
            const preview = f.last ? (f.last.mine ? 'You: ' : '') + f.last.text : '';
            const row =
                h('button.row-button.chat-row' + (f.name === current ? '.selected' : ''), {type : 'button'}, face,
                  h('div.who', h('div.strong.nowrap', f.name),
                    h('div.body.nowrap' + (f.unread ? '.fresh' : ''),
                      preview || (f.online ? (f.game ? 'In game' : 'Online') : 'Offline'))),
                  f.unread ? h('span.unread', f.unread > 99 ? '99+' : String(f.unread)) : null) as HTMLButtonElement;
            row.onclick = () => select(f.name);
            list.append(row);
        }
        if (!state.available)
            list.append(h('div.body.note', 'Messages are off right now.'));
        else if (!state.connected && state.friends.length === 0)
            list.append(h('div.body.note', 'Connecting...'));
        else if (state.friends.length === 0)
            list.append(h('div.body.note', 'Add friends to message them.'));
    }

    function renderHead() {
        const f = friend(current);
        if (!f) return;
        headAvatar.replaceChildren(avatar(36, f.name, f.avatarUrl, f.online ? 'var(--good)' : null));
        headName.textContent = f.name;
        headState.textContent = f.online ? (f.game ? 'IN GAME' : 'ONLINE') : 'OFFLINE';
        headState.className = 'chat-tag' + (f.online ? ' good' : '');
    }

    function bubble(m: ChatMessage, next: ChatMessage|undefined) {
        const el = h('div.bubble' + (m.mine ? '.mine' : '.theirs'), m.text);
        el.title = time(m.at);
        const gap = next ? new Date(next.at).getTime() - new Date(m.at).getTime() > 5 * 60_000 : true;
        return !next || next.mine !== m.mine || gap ? [ el, h('div.stamp' + (m.mine ? '.mine' : ''), time(m.at)) ]
                                                    : [ el ];
    }

    function renderStream(stick = true) {
        stream.replaceChildren();
        let lastDay = '';
        history.forEach((m, i) => {
            const d = day(m.at);
            if (d !== lastDay) {
                stream.append(h('div.chat-day', d));
                lastDay = d;
            }
            for (const el of bubble(m, history[i + 1])) stream.append(el);
        });
        if (history.length === 0) stream.append(h('div.chat-day', 'Say hi'));
        show(olderButton, more);
        if (stick) scroller.scrollTop = scroller.scrollHeight;
    }

    function renderMain() {
        const f = friend(current);
        show(empty, !f && state.available);
        show(conversation, !!f);
        show(offline, !state.available);
        offline.textContent = 'Messages are off right now.';
        if (f) renderHead();
        input.disabled = sendButton.disabled = !state.connected;
    }

    async function select(name: string) {
        current = name;
        history = [];
        more = false;
        renderList();
        renderMain();
        renderStream();
        if (sample || !account) return;
        try {
            const result = await rpc<ChatHistory>('chat.history', {id : account, with : name});
            if (current !== name) return;
            history = result.items;
            more = result.more;
            renderStream();
            markRead();
        } catch (e) {
            if (kindOf(e) !== 'canceled') shell.showToast(messageOf(e));
        }
        input.focus();
    }

    function markRead() {
        const f = friend(current);
        if (!f || !visible || sample || !account) return;
        const last = history.filter(m => !m.mine).at(-1);
        if (f.unread > 0 && last) {
            f.unread = 0;
            renderBadge();
            renderList();
            onChanged();
            rpc('chat.read', {id : account, with : f.name, upTo : last.id}).catch(() => {});
        }
    }

    olderButton.onclick = async () => {
        if (!account || !current || history.length === 0) return;
        const name = current;
        const top = scroller.scrollHeight;
        try {
            const result = await rpc<ChatHistory>('chat.history', {id : account, with : name, before : history[0].id});
            if (current !== name) return;
            history = [...result.items, ...history ];
            more = result.more;
            renderStream(false);
            scroller.scrollTop = scroller.scrollHeight - top;
        } catch (e) {
            if (kindOf(e) !== 'canceled') shell.showToast(messageOf(e));
        }
    };

    async function send() {
        const text = input.value.trim();
        if (!text || !current || !account || sample) return;
        input.value = '';
        try {
            const m = await rpc<ChatMessage>('chat.send', {id : account, to : current, text});
            if (!history.some(x => x.id === m.id && x.with === m.with) &&
                m.with.toLowerCase() === current.toLowerCase()) {
                history.push(m);
                renderStream();
            }
        } catch (e) {
            input.value = text;
            if (kindOf(e) !== 'canceled') shell.showToast(messageOf(e));
        }
    }
    sendButton.onclick = send;
    input.addEventListener('keydown', e => {
        if (e.key === 'Enter') {
            e.preventDefault();
            send();
        }
    });

    blockButton.onclick = async () => {
        if (!current || !account) return;
        const name = current;
        try {
            await rpc('social.action', {id : account, action : 'block', username : name});
            shell.showToast(`Blocked ${name}.`);
            current = null;
            renderList();
            renderMain();
        } catch (e) {
            if (kindOf(e) !== 'canceled') shell.showToast(messageOf(e));
        }
    };

    function apply(next: ChatState) {
        state = next;
        renderBadge();
        renderList();
        renderMain();
        if (current && !friend(current)) {
            current = null;
            renderMain();
        }
        onChanged();
    }

    async function load() {
        if (!account || sample) return;
        const result = await rpc<ChatState>('chat.state', {id : account}).catch(() => null);
        if (result) apply(result);
    }

    const offChanged = on('chat.changed', (s: ChatState&{id : string}) => {
        if (!sample && s.id === account) apply(s);
    });
    const offMessage = on('chat.message', (m: ChatMessage&{
                                              id: number;
                                              account: string
                                          }) => {
        if (sample || m.account !== account) return;
        const open = current && m.with.toLowerCase() === current.toLowerCase();
        if (open && !history.some(x => x.id === m.id)) {
            history.push(m);
            renderStream();
            markRead();
        }
        if (!m.mine && (!visible || !open || !document.hasFocus())) {
            rpc<{visible : boolean}>('shell.state')
                .then(s => {
                    if (!s.visible || !document.hasFocus())
                        rpc('chat.notify', {title : m.with, text : m.text}).catch(() => {});
                })
                .catch(() => {});
        }
    });

    return {
        page,
        badge,
        open(name) { select(name); },
        decorate(row, name) {
            const f = friend(name);
            const chat = h('button.chrome-button.chat-open', {type : 'button'}, '') as HTMLButtonElement;
            tooltip(chat, 'Message');
            chat.onclick = () => shell.home?.showMessages?.(name);
            const box = h('div.actions', f && f.unread ? h('span.unread', String(f.unread)) : null, chat);
            row.append(box);
        },
        unread(name) { return friend(name)?.unread ?? 0; },
        shown(v) {
            visible = v;
            if (v) {
                load();
                markRead();
            }
        },
        setAccount(id) {
            if (id === account) return;
            account = id;
            current = null;
            history = [];
            state = {available : false, connected : false, friends : []};
            renderBadge();
            renderList();
            renderMain();
            load();
        },
        showSample(s, open, h2, draft) {
            sample = true;
            state = s;
            current = open;
            history = h2;
            more = false;
            renderBadge();
            renderList();
            renderMain();
            renderStream();
            if (draft) input.value = draft;
        },
        dispose() {
            offChanged();
            offMessage();
        },
    };
}
