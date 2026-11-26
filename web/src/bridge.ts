export type ErrorKind = 'io'|'invalidData'|'invalidOperation'|'unauthorizedAccess'|'http'|'canceled'|'loginRequired'|
    'argument'|'steamDownload'|'timeout'|'win32';

export class RpcError extends Error {
    constructor(public kind: ErrorKind, message: string, public status: number) { super(message); }
}

export interface Call<T> extends Promise<T> {
    cancel(): void;
}

interface WebView {
    postMessage(message: unknown): void;
    addEventListener(type: 'message', listener: (e: {data: any}) => void): void;
}
const webview: WebView|undefined = (window as any).chrome?.webview;

let nextId = 1;
const pending = new Map < number, {
    resolve: (v: any) => void;
    reject: (e: RpcError) => void
}
>();
const listeners = new Map<string, Set<(data: any) => void>>();

webview?.addEventListener('message', e => {
    const m = e.data;
    if (m && typeof m.id === 'number') {
        const call = pending.get(m.id);
        if (!call) return;
        pending.delete(m.id);
        if (m.ok)
            call.resolve(m.result);
        else
            call.reject(new RpcError(m.error?.kind ?? 'invalidOperation', m.error?.message ?? 'Something went wrong.',
                                     m.error?.status ?? 0));
    } else if (m && typeof m.event === 'string') {
        for (const handler of [...(listeners.get(m.event) ?? [])]) handler(m.data);
    }
});

export function rpc<T = any>(method: string, params: Record<string, unknown> = {}): Call<T> {
    const id = nextId++;
    const promise = new Promise<T>((resolve, reject) => {
                        pending.set(id, {resolve, reject});
                        if (!webview)
                            reject(new RpcError('invalidOperation', 'Not running inside RWND.', 0));
                        else
                            webview.postMessage({id, method, params});
                    }) as Call<T>;
    promise.cancel = () => webview?.postMessage({cancel : id});
    return promise;
}

export function on(event: string, handler: (data: any) => void): () => void {
    let set = listeners.get(event);
    if (!set) listeners.set(event, set = new Set());
    set.add(handler);
    return () => { set!.delete(handler); };
}

export const isCanceled = (e: unknown) => e instanceof RpcError && (e.kind === 'canceled' || e.kind === 'timeout');
export const kindOf = (e: unknown) => e instanceof RpcError ? e.kind : 'invalidOperation';
export const messageOf = (e: unknown) => e instanceof Error ? e.message : String(e);
