import { DurableObject } from 'cloudflare:workers';

import { handleAdmin, isAdminPath } from './admin.js';
import { handlePublic, reply } from './api.js';
import { IncidentService } from './service.js';

export class IncidentDeskObject extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    const sql = ctx.storage.sql;
    ctx.blockConcurrencyWhile(async () => {
      this.service = new IncidentService({
        sql: {
          all: (query, ...args) => sql.exec(query, ...args).toArray(),
          get: (query, ...args) => sql.exec(query, ...args).toArray()[0],
          run: (query, ...args) => {
            sql.exec(query, ...args.map((arg) => (arg instanceof Uint8Array ? arg.buffer.slice(arg.byteOffset, arg.byteOffset + arg.byteLength) : arg)));
          },
          transaction: (work) => ctx.storage.transactionSync(work),
        },
        now: () => Date.now(),
        randomBytes: (count) => crypto.getRandomValues(new Uint8Array(count)),
        log: (text) => console.log(text),
      });
    });
  }

  file(incident, dump, account, address) {
    return this.service.file(incident, dump, account, address);
  }

  startDump(claim) {
    return this.service.startDump(claim);
  }

  appendDumpChunk(claim, offset, frameBytes) {
    return this.service.appendDumpChunk(claim, offset, new Uint8Array(frameBytes));
  }

  endDump(claim) {
    return this.service.endDump(claim);
  }

  statuses(claims) {
    return this.service.statuses(claims);
  }

  read(claim) {
    return this.service.read(claim);
  }

  feedback(feedback) {
    return this.service.feedback(feedback);
  }

  incidents() {
    return this.service.incidentsList();
  }

  incident(ticket) {
    return this.service.incidentForAdmin(ticket);
  }

  incidentDump(ticket) {
    return this.service.incidentDump(ticket);
  }

  setIncidentStatus(ticket, change) {
    return this.service.setIncidentStatus(ticket, change);
  }

  deleteIncident(ticket) {
    return this.service.deleteIncident(ticket);
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const desk = env.INCIDENTS.get(env.INCIDENTS.idFromName('global'));
    const log = (text) => console.log(text);
    if (isAdminPath(url.pathname)) return handleAdmin(request, env, desk, log);
    return handlePublic(request, env, desk, log).catch((error) => {
      log(`incidents: ${error?.message ?? error}`);
      return reply({ error: 'unavailable' }, 502);
    });
  },
};
