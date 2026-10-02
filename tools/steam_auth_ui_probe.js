// The request, protobuf encoding, HTTPS transport and response decoding below
// come from hash-pinned Valve modules. This wrapper records only result codes
// and field presence. It never forwards challenge URLs, IDs or tokens to logs.
void (async () => {
    function report(stage, result = 0, hasChallenge = false, transportError = 0) {
        document.getElementById('status').textContent = `${stage}; result ${result}; challenge present ${hasChallenge}. Account login has not been attempted.`;
        window.webkit.messageHandlers.steamAuthProbe.postMessage({stage, result, hasChallenge, transportError});
    }
    let timeout;
    try {
        const requireValve = globalThis.KleptonValveRequire;
        const auth = requireValve(13459);
        const WebAPI = requireValve(79853).D;
        const transport = new WebAPI('https://api.steampowered.com/', undefined).GetAnonymousServiceTransport();
        report('modules_ready');
        // SteamClient=1 is established by Valve's embedded enum descriptor.
        // visionOS has no verified Steam OS enum value; leave that optional
        // field unset. No account name, password or credential is supplied.
        const response = await Promise.race([auth.kX.BeginAuthSessionViaQR(transport, {
            device_friendly_name: 'Klepton on Vision Pro',
            platform_type: 1,
            device_details: {device_friendly_name: 'Klepton on Vision Pro', platform_type: 1}
        }), new Promise((_, reject) => { timeout = setTimeout(() => reject(new Error('timeout')), 8000); })]);
        clearTimeout(timeout);
        const result = response.GetEResult();
        const body = response.Body();
        const present = result === 1 && !!body.challenge_url() && !!body.client_id() && body.request_id().length > 0;
        report('qr_begin_returned', result, present, response.Hdr().transport_error());
        // Do not poll or approve a login in this transport experiment. Response
        // secrets remain ephemeral in the nonpersistent WebView and expire.
    } catch (_) {
        clearTimeout(timeout);
        report('probe_failed');
    }
})();
