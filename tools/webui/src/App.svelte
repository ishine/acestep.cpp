<script lang="ts">
	import { Volume2 } from '@lucide/svelte';
	import { app } from './lib/state.svelte.js';
	import { props } from './lib/api.js';
	import { getAllSongs } from './lib/db.js';
	import { HEALTH_POLL_MS } from './lib/config.js';
	import RequestForm from './components/RequestForm.svelte';
	import SongList from './components/SongList.svelte';
	import Toast from './components/Toast.svelte';

	// boot: load songs from IndexedDB
	$effect(() => {
		getAllSongs()
			.then((songs) => (app.songs = songs.reverse()))
			.catch(() => {});
	});

	// poll /health every HEALTH_POLL_MS, null on failure (grey labels)
	function pollProps() {
		props()
			.then((h) => (app.health = h))
			.catch(() => (app.health = null));
	}

	$effect(() => {
		pollProps();
		const id = setInterval(pollProps, HEALTH_POLL_MS);
		return () => clearInterval(id);
	});

	function statusClass(status: string | undefined): string {
		if (!app.health) return 'st-off';
		if (status === 'ok') return 'st-ok';
		if (status === 'sleeping') return 'st-sleep';
		if (status === 'disabled') return 'st-disabled';
		return 'st-off';
	}

	function onVolume(e: Event) {
		app.volume = Number((e.target as HTMLInputElement).value);
	}

	// sync dark/light class on <html> so CSS variables switch
	$effect(() => {
		document.documentElement.classList.toggle('dark', app.dark);
		document.documentElement.classList.toggle('light', !app.dark);
	});
</script>

<div class="ace-app">
	<header>
		<span class="header-label">acestep.cpp</span>
		<div class="spacer"></div>
		<label class="dark-toggle">
			<input type="checkbox" bind:checked={app.dark} /> Dark
		</label>
		<span class="status-badge {statusClass(app.health?.status.lm)}">LM</span>
		<span class="status-badge {statusClass(app.health?.status.synth)}">Synth</span>
		<div class="volume">
			<Volume2 size={14} />
			<input type="range" min="0" max="1" step="0.01" value={app.volume} oninput={onVolume} />
		</div>
	</header>

	<main>
		<section class="panel form-panel">
			<RequestForm />
		</section>
		<section class="panel songs-panel">
			<SongList />
		</section>
	</main>
</div>

<Toast />

<style>
	:global(:root) {
		--bg: #1a1a2e;
		--bg-input: #16213e;
		--bg-card: #1e2a45;
		--bg-btn: #0f3460;
		--bg-btn-hover: #1a4a8a;
		--fg: #fff;
		--fg-dim: #8a8aa0;
		--border: #2a2a4a;
		--focus: #2ed573;
		--error: #ff6b6b;
		--color-ok: #2ed573;
		--color-sleep: #ffa502;
		--color-disabled: #ff4757;
		--color-off: #555;
		--waveform-dim: #4a4a6a;
		--waveform-play: #2ed573;
		color-scheme: dark;
	}
	:global(:root.light) {
		--bg: #f5f5f5;
		--bg-input: #fff;
		--bg-card: #fff;
		--bg-btn: #e0e0e0;
		--bg-btn-hover: #d0d0d0;
		--fg: #000;
		--fg-dim: #666;
		--border: #ccc;
		--focus: #27ae60;
		--error: #c0392b;
		--color-ok: #27ae60;
		--color-sleep: #e67e22;
		--color-disabled: #e74c3c;
		--color-off: #bbb;
		--waveform-dim: #ccc;
		--waveform-play: #27ae60;
		color-scheme: light;
	}
	:global(*, *::before, *::after) {
		box-sizing: border-box;
		margin: 0;
	}
	:global(body) {
		font-family:
			system-ui,
			-apple-system,
			sans-serif;
		background: var(--bg);
		color: var(--fg);
		min-height: 100dvh;
	}
	.ace-app {
		display: flex;
		flex-direction: column;
		min-height: 100dvh;
	}
	header {
		display: flex;
		align-items: center;
		gap: 0.6rem;
		padding: 0.5rem 1rem;
		border-bottom: 1px solid var(--border);
	}
	.header-label {
		font-size: 1.1rem;
		font-weight: 600;
		color: var(--fg);
	}
	.status-badge {
		font-size: 0.7rem;
		font-weight: 600;
		font-family: monospace;
		padding: 0.1rem 0.4rem;
		border-radius: 3px;
		color: #000;
	}
	.st-ok {
		background: var(--color-ok);
	}
	.st-sleep {
		background: var(--color-sleep);
	}
	.st-disabled {
		background: var(--color-disabled);
	}
	.st-off {
		background: var(--color-off);
	}
	.spacer {
		flex: 1;
	}
	.dark-toggle {
		display: flex;
		align-items: center;
		gap: 0.3rem;
		font-size: 0.75rem;
		color: var(--fg-dim);
		cursor: pointer;
	}
	.dark-toggle {
		display: flex;
		align-items: center;
		gap: 0.3rem;
		font-size: 0.75rem;
		color: var(--fg);
		cursor: pointer;
	}
	.volume {
		display: flex;
		align-items: center;
		gap: 0.3rem;
		color: var(--fg);
	}
	.volume input[type='range'] {
		width: 80px;
		cursor: pointer;
	}
	main {
		flex: 1;
		display: flex;
		gap: 1px;
		background: var(--border);
		overflow: hidden;
	}
	.panel {
		background: var(--bg);
		padding: 1rem;
		overflow-y: auto;
	}
	.form-panel {
		width: 400px;
		flex-shrink: 0;
	}
	.songs-panel {
		flex: 1;
	}
	@media (max-width: 800px) {
		main {
			flex-direction: column;
		}
		.form-panel {
			width: 100%;
		}
	}
</style>
