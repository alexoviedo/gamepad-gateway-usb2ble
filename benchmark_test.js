import fs from 'fs';
import { performance } from 'perf_hooks';

const content = fs.readFileSync('webapp/validation.js', 'utf8');

// We will mock the required parts of the file to benchmark computeMappedOutputs
// Actually, it's easier to run it via JSDOM or just copy the relevant functions
