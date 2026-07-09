import type { RunStore } from "./store.js";
import type { RunEvent } from "./types.js";

export type RunEventListener = (event: RunEvent) => void;

// Live runs keep their events in memory for fast SSE replay; archived runs
// are read from the store on demand so memory stays bounded.
export class EventLog {
  readonly #events = new Map<string, RunEvent[]>();
  readonly #listeners = new Set<RunEventListener>();
  readonly #store: RunStore | undefined;
  #nextId: number;

  constructor(options: { store?: RunStore; nextId?: number } = {}) {
    this.#store = options.store;
    this.#nextId = options.nextId ?? 1;
  }

  append(runId: string, type: RunEvent["type"], payload: Record<string, unknown>): RunEvent {
    const event: RunEvent = {
      id: `evt_${this.#nextId++}`,
      runId,
      type,
      timestamp: new Date().toISOString(),
      payload
    };
    this.#remember(event);
    this.#store?.appendEvent(event);

    for (const listener of this.#listeners) {
      listener(event);
    }

    return event;
  }

  list(runId: string): RunEvent[] {
    const inMemory = this.#events.get(runId);
    if (inMemory) {
      return [...inMemory];
    }

    return this.#store?.listEvents(runId) ?? [];
  }

  /** Forget a run's events entirely (memory; the store row removal is separate). */
  drop(runId: string): void {
    this.#events.delete(runId);
  }

  /** Release the in-memory copy; future reads fall through to the store. */
  evict(runId: string): void {
    this.#events.delete(runId);
  }

  subscribe(listener: RunEventListener): () => void {
    this.#listeners.add(listener);

    return () => {
      this.#listeners.delete(listener);
    };
  }

  #remember(event: RunEvent): void {
    const events = this.#events.get(event.runId) ?? [];
    events.push(event);
    this.#events.set(event.runId, events);
  }
}
