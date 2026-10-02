// Use Valve's pinned authentication service, RSA helper and HTTPS transport.
// Secrets go only to the native credential mailbox, never the diagnostic bridge.
void (() => {
    const auth = KleptonValveRequire(13459).kX;
    const WebAPI = KleptonValveRequire(79853).D;
    const transport = new WebAPI('https://api.steampowered.com/', undefined).GetAnonymousServiceTransport();
    let generation = 0, running = false, guardSession, submittingCode = false;
    const send = payload => window.webkit.messageHandlers.steamLogin.postMessage(payload);
    const status = (stage, result = 0) => send({kind: 'status', stage, result});
    async function bounded(operation) {
        let timer;
        try { return await Promise.race([operation, new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error('timeout')), 10000);
        })]); } finally { clearTimeout(timer); }
    }
    async function poll(run, body) {
        let clientID = body.client_id(), requestID = body.request_id();
        if (!clientID || !requestID.length) { status('failure'); return; }
        const interval = Math.max(1000, Math.min(30000, body.interval() * 1000));
        const deadline = Date.now() + 300000;
        while (generation === run && Date.now() < deadline) {
            await new Promise(resolve => setTimeout(resolve, interval));
            if (generation !== run) return;
            const response = await bounded(auth.PollAuthSessionStatus(transport, {client_id: clientID, request_id: requestID}));
            if (generation !== run) return;
            const result = response.GetEResult();
            if (result !== 1) { status('failure', result); return; }
            const update = response.Body();
            if (update.new_client_id() && update.new_client_id() !== '0') {
                clientID = update.new_client_id();
                if (guardSession?.run === run) guardSession.clientID = clientID;
            }
            if (update.new_challenge_url()) send({kind: 'challenge', url: update.new_challenge_url()});
            if (update.agreement_session_url()) { status('agreement_required'); return; }
            if (update.refresh_token() && update.account_name()) {
                send({kind: 'credential', refreshToken: update.refresh_token(), accountName: update.account_name()});
                status('token_delivered', result); return;
            }
            status('poll_waiting', result);
        }
        if (generation === run) status('expired');
    }
    async function start(mode, account = '', password = '') {
        if (running) return;
        running = true; const run = ++generation;
        try {
            status('starting');
            let response;
            const device = {device_friendly_name: 'Klepton on Vision Pro', platform_type: 1};
            if (mode === 'qr') {
                response = await bounded(auth.BeginAuthSessionViaQR(transport, {...device, device_details: device}));
            } else {
                if (!account || account.length > 64 || !password || password.length > 1024) { status('failure'); return; }
                const key = await bounded(auth.GetPasswordRSAPublicKey(transport, {account_name: account}));
                if (generation !== run) return;
                if (key.GetEResult() !== 1) { status('failure', key.GetEResult()); return; }
                const rsa = key.Body();
                // This is the same RSA implementation used by Valve's login UI.
                const helper = KleptonValveRequire(36500).A;
                const encrypted = helper.encrypt(password, helper.getPublicKey(rsa.publickey_mod(), rsa.publickey_exp()));
                password = '';
                if (!encrypted) { status('failure'); return; }
                response = await bounded(auth.BeginAuthSessionViaCredentials(transport, {
                    ...device, device_details: device, account_name: account, encrypted_password: encrypted,
                    encryption_timestamp: rsa.timestamp(), remember_login: true, persistence: 1
                }));
            }
            if (generation !== run) return;
            const result = response.GetEResult();
            if (result !== 1) { status('failure', result); return; }
            const body = response.Body();
            if (mode === 'qr') {
                if (!body.challenge_url()) { status('failure'); return; }
                send({kind: 'challenge', url: body.challenge_url()}); status('approval_required', result);
            } else {
                if (body.agreement_session_url()) { status('agreement_required'); return; }
                const allowed = body.allowed_confirmations().map(c => c.confirmation_type());
                const types = allowed.filter(t => t === 2 || t === 3);
                if (types.length) {
                    guardSession = {run, clientID: body.client_id(), steamID: body.steamid(), types};
                    send({kind: 'guard_options', device: types.includes(3), email: types.includes(2)});
                    status('code_required', result);
                } else if (allowed.includes(4) || allowed.includes(5)) {
                    status('confirmation_required', result);
                } else if (!allowed.includes(1)) { status('unsupported_guard'); return; }
            }
            await poll(run, body);
        } catch (_) {
            if (generation === run) status('failure');
        } finally {
            password = ''; account = '';
            if (generation === run) { running = false; guardSession = undefined; submittingCode = false; }
        }
    }
    globalThis.KleptonSteamLogin = {
        start: () => start('qr'),
        startCredentials: (account, password) => start('credentials', account, password),
        async submitGuardCode(code, type) {
            const session = guardSession;
            if (!session || session.run !== generation || submittingCode || !session.types.includes(type) || !/^[A-Z0-9]{5}$/.test(code)) return;
            submittingCode = true;
            try {
                const operation = auth.UpdateAuthSessionWithSteamGuardCode(transport, {
                    client_id: session.clientID, steamid: session.steamID, code, code_type: type
                });
                code = '';
                const response = await bounded(operation);
                if (session.run !== generation || guardSession !== session) return;
                status(response.GetEResult() === 1 ? 'code_accepted' : 'code_rejected', response.GetEResult());
            } catch (_) { if (session.run === generation && guardSession === session) status('code_rejected'); }
            finally { code = ''; if (session.run === generation) submittingCode = false; }
        },
        cancel() { ++generation; running = false; guardSession = undefined; submittingCode = false; status('cancelled'); }
    };
    status('modules_ready');
})();
