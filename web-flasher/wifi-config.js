function validateToken(value, fieldName, { allowEmpty = false } = {}) {
    const v = String(value ?? '');
    if (!allowEmpty && !v.trim()) {
        throw new Error(`${fieldName} is required.`);
    }
    if (v.includes(' ') || /[\r\n\t]/.test(v)) {
        throw new Error(`${fieldName} cannot contain spaces.`);
    }
    return v;
}

export function buildWifiConfigCommands({ ssid, password = '', rebootAfter = true } = {}) {
    const safeSsid = validateToken(ssid, 'SSID');
    const safePassword = validateToken(password, 'Password', { allowEmpty: true });

    const commands = [];
    if (safePassword) {
        commands.push(`wifi set ${safeSsid} ${safePassword}`);
    } else {
        commands.push(`wifi set ${safeSsid}`);
    }
    if (rebootAfter) commands.push('reboot');
    return commands;
}
