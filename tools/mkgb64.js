// Build gb.v64 / gbc.v64 exactly as lambertjamesd's romwrapper "Download Emulator"
// button does, by running that page's own JavaScript rather than reimplementing it.
// The button is downloadEV(): fetch gb.n64, convert() it with the GB64_ROM magic as
// the "rom" and no boot ROMs, then write the identical result to both filenames.
const fs = require('fs');

const html = fs.readFileSync(process.env.RW || '/tmp/rw.html', 'utf8');
const crcjs = fs.readFileSync(process.env.CRC || '/tmp/crc.js', 'utf8');

// Lift a named function out of the page by brace matching, so the code that runs is
// character-for-character the code the site serves.
function lift(src, name) {
    const start = src.indexOf('function ' + name);
    if (start < 0) throw new Error('not found: ' + name);
    let i = src.indexOf('{', start), depth = 0;
    for (let j = i; j < src.length; ++j) {
        if (src[j] === '{') depth++;
        else if (src[j] === '}' && --depth === 0) return src.slice(start, j + 1);
    }
    throw new Error('unbalanced: ' + name);
}

const sandbox = {};
// reportError and outputCRC only touch the DOM to show progress; stub them but make
// reportError loud, because "could not find rom insertion location" must not pass silently.
const prelude = `
    let errors = [];
    function reportError(m) { errors.push(m); }
    function outputCRC() {}
`;

const code = [
    crcjs,
    prelude,
    lift(html, 'findIndex'),
    lift(html, 'copyInto'),
    lift(html, 'convert'),
    "const edSavetypes = [0x50, 0x30];",
    `
    const template = new Uint8Array(fs.readFileSync('/tmp/gb.n64'));
    const magic = new Uint8Array([0x47,0x42,0x36,0x34,0x5f,0x52,0x4f,0x4d]); // 'GB64_ROM'
    const result = convert(template, 'gb64', magic, undefined, undefined, 0);
    if (!result) { console.error('convert returned nothing; errors:', errors); process.exit(1); }
    module.exports = { result, errors, romIndex: findIndex(template, 'GB64_ROM') };
    `,
].join('\n');

const mod = { exports: {} };
new Function('fs', 'module', 'console', 'process', code)(fs, mod, console, process);

const { result, errors, romIndex } = mod.exports;
console.log('template bytes :', fs.statSync('/tmp/gb.n64').size);
console.log('GB64_ROM at    : 0x' + romIndex.toString(16));
console.log('output bytes   :', result.length, '(0x' + result.length.toString(16) + ')');
console.log('errors         :', errors.length ? errors : 'none');
const out = Buffer.from(result);
fs.writeFileSync('/tmp/gb.v64', out);
fs.writeFileSync('/tmp/gbc.v64', out);
console.log('CRC1/CRC2      :', out.subarray(0x10, 0x18).toString('hex'));
console.log('ED bytes 3C/3D/3F:', out[0x3c].toString(16), out[0x3d].toString(16), out[0x3f].toString(16));
