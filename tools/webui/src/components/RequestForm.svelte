<script lang="ts">
	import { onMount } from 'svelte';
	import { RotateCcw, Download, FolderOpen } from '@lucide/svelte';
	import { app, toast, setRequest } from '../lib/state.svelte.js';
	import { rollDice } from '../lib/dice.js';
	import {
		lmSubmit,
		lmSubmitInspire,
		lmSubmitFormat,
		synthSubmit,
		synthSubmitWithAudio,
		pollJob,
		jobResultJson,
		jobResultBlobs,
		cancelJob
	} from '../lib/api.js';
	import { putSong, getAllSongs, saveJob, loadJob, loadJobId, clearJob } from '../lib/db.js';
	import {
		TASK_COVER,
		TASK_COVER_NOFSQ,
		TASK_REPAINT,
		TASK_LEGO,
		TASK_EXTRACT,
		TASK_COMPLETE,
		TRACK_NAMES
	} from '../lib/config.js';
	import type { AceRequest, Song } from '../lib/types.js';

	let busyLm = $state(false);
	let busySynth = $state(false);
	let busy = $derived(busyLm || busySynth);
	let fileInput: HTMLInputElement;

	let d = $derived(app.props?.default);
	let ditModels = $derived(app.props?.models.dit ?? []);
	let lmModels = $derived(app.props?.models.lm ?? []);
	let loraList = $derived(app.props?.loras ?? []);
	let loraStale = $derived(!!app.request.lora && !loraList.includes(String(app.request.lora)));
	let taskType = $derived(app.request.task_type || '');
	let dp = $derived(
		app.props?.presets
			? String(app.request.synth_model || '').includes('turbo')
				? app.props.presets.turbo
				: app.props.presets.sft
			: null
	);
	let needsTrack = $derived(
		taskType === TASK_LEGO || taskType === TASK_EXTRACT || taskType === TASK_COMPLETE
	);
	let singleTrack = $derived(taskType === TASK_LEGO || taskType === TASK_EXTRACT);

	// fill number fields with server defaults (avoids empty inputs)
	$effect(() => {
		if (!d) return;
		if (app.request.lm_batch_size == null) app.request.lm_batch_size = d.lm_batch_size;
		if (app.request.synth_batch_size == null) app.request.synth_batch_size = d.synth_batch_size;
		if (app.request.peak_clip == null) app.request.peak_clip = d.peak_clip;
	});

	// DiT input indicators
	let hasCodes = $derived(!!app.request.audio_codes?.trim() && app.srcSongId == null);
	let hasSrc = $derived(app.srcSongId != null);
	let hasRange = $derived(app.srcRangeStart >= 0 && app.srcRangeEnd > app.srcRangeStart);
	let hasRef = $derived(app.refSongId != null);

	// instrumental mode: checked when lyrics and language match the convention.
	// any manual edit to either field naturally unchecks via $derived.
	let instrumental = $derived(
		String(app.request.lyrics || '').trim() === '[Instrumental]' &&
			String(app.request.vocal_language || '').trim() === 'unknown'
	);

	function toggleInstrumental() {
		if (instrumental) {
			app.request.lyrics = '';
			app.request.vocal_language = '';
		} else {
			app.request.lyrics = '[Instrumental]';
			app.request.vocal_language = 'unknown';
		}
	}

	// track selection: radio for lego/extract, multi for complete
	let selectedTracks: Set<string> = $state(new Set());

	function toggleTrack(name: string) {
		let next = new Set(selectedTracks);
		if (next.has(name)) {
			next.delete(name);
		} else {
			if (singleTrack) next.clear();
			next.add(name);
		}
		selectedTracks = next;
	}

	// sync set to request string (preserve TRACK_NAMES order)
	$effect(() => {
		app.request.track = TRACK_NAMES.filter((n: string) => selectedTracks.has(n)).join(' | ');
	});

	// clear tracks when task has no use for them, trim to 1 for radio modes
	$effect(() => {
		if (!needsTrack) {
			if (selectedTracks.size > 0) selectedTracks = new Set();
		} else if (singleTrack && selectedTracks.size > 1) {
			selectedTracks = new Set([...selectedTracks].slice(0, 1));
		}
	});

	// cancel the active pipeline job
	async function cancelPipeline() {
		try {
			if (busySynth) {
				const synthId = loadJobId('synth');
				if (synthId) await cancelJob(synthId);
			} else if (busyLm) {
				const lmId = loadJobId('lm');
				if (lmId) await cancelJob(lmId);
			}
		} catch {}
	}

	// on mount: resume polling for any pending jobs in localStorage.
	onMount(() => {
		// LM job
		const lmId = loadJobId('lm');
		if (lmId) {
			busyLm = true;
			pollJob(lmId)
				.then(() => jobResultJson(lmId))
				.then((results) => {
					clearJob('lm');
					app.pendingRequests = results;
					app.pendingIndex = 0;
					if (results.length > 0) {
						setRequest(results[0]);
					}
				})
				.catch(() => {
					clearJob('lm');
				})
				.finally(() => {
					busyLm = false;
				});
		}

		// synth job
		const synthJob = loadJob('synth');
		if (synthJob) {
			busySynth = true;
			pollJob(synthJob.id)
				.then(() => jobResultBlobs(synthJob.id))
				.then(async (blobs) => {
					clearJob('synth');
					const now = Date.now();
					for (let i = blobs.length - 1; i >= 0; i--) {
						const t = synthJob.tracks[i] || {
							caption: '',
							seed: 0,
							duration: 0,
							task: '',
							request: { caption: '' }
						};
						const suffix = [synthJob.variant, t.task].filter((s) => s).join(' ');
						const song: Song = {
							name: suffix ? synthJob.name + ' (' + suffix + ')' : synthJob.name,
							format: synthJob.format,
							created: now + i,
							caption: t.caption,
							seed: t.seed,
							duration: t.duration,
							request: t.request,
							audio: blobs[i]
						};
						await putSong(song);
					}
					app.songs = (await getAllSongs()).reverse();
				})
				.catch(() => {
					clearJob('synth');
				})
				.finally(() => {
					busySynth = false;
				});
		}
	});

	function reset() {
		app.name = '';
		setRequest({ caption: '' });
		app.pendingRequests = [];
		app.pendingIndex = 0;
		selectedTracks = new Set();
	}

	function exportJson() {
		const json = JSON.stringify(buildRequest(), null, 2);
		const blob = new Blob([json], { type: 'application/json' });
		const url = URL.createObjectURL(blob);
		const a = document.createElement('a');
		a.href = url;
		const safe = app.name.replace(/[\\/:*?"<>|\x00-\x1f]/g, '') || 'request';
		a.download = `${safe}.json`;
		a.click();
		URL.revokeObjectURL(url);
	}

	function importJson() {
		fileInput.click();
	}

	function onFileSelected(e: Event) {
		const input = e.target as HTMLInputElement;
		const file = input.files?.[0];
		if (!file) return;
		// reset so the same file can be re-opened
		input.value = '';

		const ext = file.name.split('.').pop()?.toLowerCase() || '';

		// JSON: load request into form (existing behavior)
		if (ext === 'json') {
			file
				.text()
				.then((text) => {
					setRequest(JSON.parse(text) as AceRequest);
					app.name = file.name.replace(/\.json$/i, '') || 'Imported';
					app.pendingRequests = [];
					app.pendingIndex = 0;
				})
				.catch(() => {
					toast('Invalid JSON file');
				});
			return;
		}

		// MP3 or WAV: create song card (audio only, use Scan for metadata)
		if (ext === 'mp3' || ext === 'wav') {
			openAudio(file, ext);
			return;
		}

		toast('Unsupported file type: ' + ext);
	}

	// open audio file: create song card with audio only (no server call).
	// use Scan on the card to analyze metadata.
	async function openAudio(file: File, ext: string) {
		const blob = new Blob([await file.arrayBuffer()], {
			type: ext === 'wav' ? 'audio/wav' : 'audio/mpeg'
		});
		const name = file.name.replace(/\.(mp3|wav)$/i, '') || 'Imported';
		const song: Song = {
			name,
			format: ext,
			created: Date.now(),
			caption: '',
			seed: 0,
			duration: 0,
			request: { caption: '' },
			audio: blob
		};
		song.id = await putSong(song);
		app.songs.unshift(song);
		app.name = name;
		toast('Opened: ' + name, 4000, true);
	}

	// convert string or number to number, return undefined if empty/NaN
	function num(v: unknown): number | undefined {
		if (v == null || v === '') return undefined;
		const n = Number(v);
		return isNaN(n) ? undefined : n;
	}

	// snapshot app.request into a clean AceRequest with proper types.
	// bind:value guarantees app.request always matches the DOM.
	function buildRequest(): AceRequest {
		const r = app.request;
		const out: AceRequest = { caption: String(r.caption || '') };
		if (r.lyrics) out.lyrics = String(r.lyrics);
		if (r.audio_codes) out.audio_codes = String(r.audio_codes);
		if (r.vocal_language) out.vocal_language = String(r.vocal_language);
		if (r.keyscale) out.keyscale = String(r.keyscale);
		if (r.timesignature) out.timesignature = String(r.timesignature);
		const bpm = num(r.bpm);
		if (bpm != null) out.bpm = bpm;
		const duration = num(r.duration);
		if (duration != null) out.duration = duration;
		const seed = num(r.seed);
		if (seed != null) out.seed = seed;
		const lm_temperature = num(r.lm_temperature);
		if (lm_temperature != null) out.lm_temperature = lm_temperature;
		const lm_cfg_scale = num(r.lm_cfg_scale);
		if (lm_cfg_scale != null) out.lm_cfg_scale = lm_cfg_scale;
		const lm_top_p = num(r.lm_top_p);
		if (lm_top_p != null) out.lm_top_p = lm_top_p;
		const lm_top_k = num(r.lm_top_k);
		if (lm_top_k != null) out.lm_top_k = lm_top_k;
		if (r.lm_negative_prompt) out.lm_negative_prompt = String(r.lm_negative_prompt);
		const inference_steps = num(r.inference_steps);
		if (inference_steps != null) out.inference_steps = inference_steps;
		const guidance_scale = num(r.guidance_scale);
		if (guidance_scale != null) out.guidance_scale = guidance_scale;
		const shift = num(r.shift);
		if (shift != null) out.shift = shift;
		const audio_cover_strength = num(r.audio_cover_strength);
		if (audio_cover_strength != null) out.audio_cover_strength = audio_cover_strength;
		const cover_noise_strength = num(r.cover_noise_strength);
		if (cover_noise_strength != null) out.cover_noise_strength = cover_noise_strength;
		const lm_batch_size = num(r.lm_batch_size);
		if (lm_batch_size != null && lm_batch_size >= 1) out.lm_batch_size = lm_batch_size;
		const synth_batch_size = num(r.synth_batch_size);
		if (synth_batch_size != null && synth_batch_size >= 1) out.synth_batch_size = synth_batch_size;
		if (r.task_type) out.task_type = String(r.task_type);
		if (r.track) out.track = String(r.track);
		if (r.infer_method) out.infer_method = String(r.infer_method);
		if (r.synth_model) out.synth_model = String(r.synth_model);
		if (r.lm_model) out.lm_model = String(r.lm_model);
		if (r.lora && loraList.includes(String(r.lora))) out.lora = String(r.lora);
		const lora_scale = num(r.lora_scale);
		if (lora_scale != null) out.lora_scale = lora_scale;
		return out;
	}

	// save current form edits back into pendingRequests[pendingIndex]
	function savePending() {
		if (app.pendingRequests.length > 0 && app.pendingIndex < app.pendingRequests.length) {
			app.pendingRequests[app.pendingIndex] = buildRequest();
		}
	}

	// load pendingRequests[index] into the form.
	// synth params are form-global, not per-pending: preserve them across switches.
	function loadPending(index: number) {
		const r = app.pendingRequests[index];
		setRequest({
			...r,
			inference_steps: app.request.inference_steps,
			guidance_scale: app.request.guidance_scale,
			shift: app.request.shift,
			seed: app.request.seed,
			audio_cover_strength: app.request.audio_cover_strength,
			cover_noise_strength: app.request.cover_noise_strength,
			synth_batch_size: app.request.synth_batch_size
		});
		app.pendingIndex = index;
	}

	// switch to a different pending composition (saves current edits first)
	function switchPending(delta: number) {
		const next = app.pendingIndex + delta;
		if (next < 0 || next >= app.pendingRequests.length) return;
		savePending();
		loadPending(next);
	}

	// shared: call an LM endpoint via the job system and load results into the form.
	async function lmCall(fn: (req: AceRequest) => Promise<string>) {
		busyLm = true;
		try {
			const req = buildRequest();
			req.audio_codes = '';
			const jobId = await fn(req);
			saveJob('lm', jobId);
			await pollJob(jobId);
			const results = await jobResultJson(jobId);
			clearJob('lm');
			if (results.length > 0) {
				app.pendingRequests = results;
				app.pendingIndex = 0;
				setRequest({
					...results[0],
					inference_steps: app.request.inference_steps,
					guidance_scale: app.request.guidance_scale,
					shift: app.request.shift,
					seed: app.request.seed,
					audio_cover_strength: app.request.audio_cover_strength,
					cover_noise_strength: app.request.cover_noise_strength,
					repaint_strength: app.request.repaint_strength,
					synth_batch_size: app.request.synth_batch_size,
					lm_batch_size: app.request.lm_batch_size,
					lm_temperature: app.request.lm_temperature,
					lm_cfg_scale: app.request.lm_cfg_scale,
					lm_top_p: app.request.lm_top_p,
					lm_top_k: app.request.lm_top_k,
					lm_negative_prompt: app.request.lm_negative_prompt
				});
			}
		} catch (e: unknown) {
			toast(e instanceof Error ? e.message : String(e));
		} finally {
			busyLm = false;
		}
	}

	// Dice: pick a random example prompt and fill the caption
	function dice() {
		setRequest(rollDice());
	}

	// Inspire: short caption -> fresh metadata + lyrics (no audio codes)
	async function inspire() {
		await lmCall(lmSubmitInspire);
	}

	// Format: caption + lyrics -> metadata + lyrics (no audio codes)
	async function format() {
		await lmCall(lmSubmitFormat);
	}

	// Compose: send form to LM, store all enriched results for batch synth.
	// The LM preserves user-provided fields and fills the rest independently
	// per batch item. Each result is a complete standalone request.
	async function compose() {
		await lmCall(lmSubmit);
	}

	// POST /synth: send pending requests (or current form) to the server.
	// synth params (batch, seed, steps, CFG, shift) come from the form, not from pending.
	// server groups by request and expands synth_batch_size for GPU batching.
	// webui resolves seeds and predicts the expanded list for SongCard mapping.
	async function synthesize() {
		busySynth = true;
		try {
			savePending();
			const reqs: AceRequest[] =
				app.pendingRequests.length > 0 ? $state.snapshot(app.pendingRequests) : [buildRequest()];

			// read synth params from the form (global, not per-pending).
			const synthBatch = Math.max(1, Number(app.request.synth_batch_size) || 1);
			const userSeed = num(app.request.seed);
			const hasSeed = userSeed != null && userSeed >= 0;
			const synthParams: Partial<AceRequest> = {};
			const steps = num(app.request.inference_steps);
			if (steps != null) synthParams.inference_steps = steps;
			const cfg = num(app.request.guidance_scale);
			if (cfg != null) synthParams.guidance_scale = cfg;
			const sh = num(app.request.shift);
			if (sh != null) synthParams.shift = sh;
			const acs = num(app.request.audio_cover_strength);
			if (acs != null) synthParams.audio_cover_strength = acs;
			const cns = num(app.request.cover_noise_strength);
			if (cns != null) synthParams.cover_noise_strength = cns;
			const rps = num(app.request.repaint_strength);
			if (rps != null) synthParams.repaint_strength = rps;
			// task_type and track from form
			const t = app.request.task_type || '';
			if (t) synthParams.task_type = t;
			if (app.request.track) synthParams.track = app.request.track;
			// infer_method from form
			const im = app.request.infer_method || '';
			if (im) synthParams.infer_method = im;
			const b = num(app.request.peak_clip);
			if (b != null) synthParams.peak_clip = b;
			// model routing from form
			if (app.request.synth_model) synthParams.synth_model = app.request.synth_model;
			if (app.request.lora && loraList.includes(String(app.request.lora)))
				synthParams.lora = app.request.lora;
			const loraScale = num(app.request.lora_scale);
			if (loraScale != null) synthParams.lora_scale = loraScale;
			// repaint/lego: inject range from source audio selection (optional for lego)
			if (
				(t === TASK_REPAINT || t === TASK_LEGO) &&
				app.srcRangeStart >= 0 &&
				app.srcRangeEnd > app.srcRangeStart
			) {
				synthParams.repainting_start = app.srcRangeStart;
				synthParams.repainting_end = app.srcRangeEnd;
			}

			// resolve seeds, build server payload and local expanded list for SongCard mapping.
			// server receives synth_batch_size and expands internally (groups by T for GPU batch).
			// webui predicts the same expansion: seed, seed+1, ..., seed+N-1.
			const toSend: AceRequest[] = [];
			const expanded: AceRequest[] = [];
			for (const r of reqs) {
				const base = hasSeed ? userSeed : Math.floor(Math.random() * 0x100000000);
				toSend.push({ ...r, ...synthParams, seed: base, synth_batch_size: synthBatch });
				for (let i = 0; i < synthBatch; i++) {
					expanded.push({ ...r, ...synthParams, seed: base + i });
				}
			}

			// find source audio (cover/lego/repaint) and reference audio (timbre)
			const srcSong = app.srcSongId != null ? app.songs.find((s) => s.id === app.srcSongId) : null;
			const refSong = app.refSongId != null ? app.songs.find((s) => s.id === app.refSongId) : null;

			// extract DiT variant from model filename
			// "acestep-v15-xl-turbo-Q8_0.gguf" -> "xl-turbo"
			const model = String(app.request.synth_model || '');
			const vm = model.match(/^acestep-v15-(.+?)-(Q\d.*|BF16)\.gguf$/);
			const variant = vm ? vm[1] : '';
			const baseName = app.name || 'Untitled';

			// submit job, poll until done, fetch result
			const jobId =
				srcSong || refSong
					? await synthSubmitWithAudio(
							toSend,
							srcSong?.audio ?? null,
							refSong?.audio ?? null,
							app.format
						)
					: await synthSubmit(toSend, app.format);
			saveJob('synth', {
				id: jobId,
				name: baseName,
				format: app.format,
				variant,
				tracks: expanded.map((r) => ({
					caption: r.caption || '',
					seed: r.seed || 0,
					duration: r.duration || 0,
					task: r.task_type || 'text2music',
					request: r
				}))
			});
			await pollJob(jobId);
			const blobs = await jobResultBlobs(jobId);
			clearJob('synth');

			const now = Date.now();

			for (let i = blobs.length - 1; i >= 0; i--) {
				const r = expanded[i];
				const task = r.task_type || 'text2music';
				const suffix = [variant, task].filter((s) => s).join(' ');
				const song = {
					name: suffix ? baseName + ' (' + suffix + ')' : baseName,
					format: app.format,
					created: now + i,
					caption: r.caption,
					seed: r.seed || 0,
					duration: r.duration || 0,
					request: r,
					audio: blobs[i]
				} as Song;
				song.id = await putSong(song);
				app.songs.unshift(song);
			}
			app.pendingRequests = [];
			app.pendingIndex = 0;
		} catch (e: unknown) {
			toast(e instanceof Error ? e.message : String(e));
		} finally {
			busySynth = false;
		}
	}

	function ph(v: unknown): string {
		return v != null ? String(v) : '';
	}
</script>

<form class="request-form" onsubmit={(e) => e.preventDefault()}>
	<input
		type="file"
		accept=".json,.mp3,.wav"
		bind:this={fileInput}
		onchange={onFileSelected}
		hidden
	/>
	<div class="toolbar">
		<button type="button" onclick={importJson} title="Open JSON prompt, MP3 or WAV"
			><FolderOpen size={14} /> Open</button
		>
		<button type="button" onclick={exportJson} title="Save JSON prompt"
			><Download size={14} /> Save</button
		>
		<button type="button" onclick={reset} title="Reset prompt"><RotateCcw size={14} /> Reset</button
		>
	</div>

	<details>
		<summary>Models</summary>
		<div class="details-body">
			<div class="model-row">
				<span class="model-label">LM</span>
				<select bind:value={app.request.lm_model}>
					{#each lmModels as name}
						<option value={name}>{name}</option>
					{/each}
				</select>
			</div>
			<div class="model-row">
				<span class="model-label">DiT</span>
				<select bind:value={app.request.synth_model}>
					{#each ditModels as name}
						<option value={name}>{name}</option>
					{/each}
				</select>
			</div>
			<div class="model-row">
				<span class="model-label">LoRA</span>
				<select bind:value={app.request.lora}>
					<option value="">Disabled</option>
					{#if loraStale}
						<option value={app.request.lora} disabled>{app.request.lora}</option>
					{/if}
					{#each loraList as name}
						<option value={name}>{name}</option>
					{/each}
				</select>
				<input
					type="text"
					class="batch-input"
					placeholder="1.0"
					bind:value={app.request.lora_scale}
				/>
			</div>
		</div>
	</details>

	<div class="section-title">Name</div>
	<input type="text" bind:value={app.name} placeholder="Untitled" />

	<div class="section-title">Caption</div>
	<textarea
		rows="8"
		placeholder="Upbeat pop rock with driving guitars... (the only required field, may be enriched by the LM)"
		bind:value={app.request.caption}
	></textarea>

	<div class="section-title lyrics-header">
		Lyrics
		<label class="instrumental-toggle" title="Set lyrics to [Instrumental] and language to unknown">
			<input type="checkbox" checked={instrumental} onchange={toggleInstrumental} /> Instrumental
		</label>
	</div>
	<textarea
		rows="8"
		placeholder="Write your own lyrics or leave empty to let the LM create them..."
		bind:value={app.request.lyrics}
	></textarea>

	<div class="meta-grid">
		<label
			>Language <input
				type="text"
				placeholder={ph(d?.vocal_language)}
				bind:value={app.request.vocal_language}
			/></label
		>
		<label>BPM <input type="text" placeholder={ph(d?.bpm)} bind:value={app.request.bpm} /></label>
		<label
			>Duration <input
				type="text"
				placeholder={ph(d?.duration)}
				bind:value={app.request.duration}
			/></label
		>
		<label
			>Key <input
				type="text"
				placeholder={ph(d?.keyscale)}
				bind:value={app.request.keyscale}
			/></label
		>
		<label
			>Time sig <input
				type="text"
				placeholder={ph(d?.timesignature)}
				bind:value={app.request.timesignature}
			/></label
		>
	</div>

	<div class="lm-row">
		<button
			type="button"
			disabled={busy}
			onclick={dice}
			title="Pick a random example from ACE-Step sample prompts. Use Inspire next to complete missing fields."
			>Dice</button
		>
		<button
			type="button"
			disabled={busy}
			onclick={inspire}
			title="Step 1: LM inference to generate metadata and lyrics from your caption. Next: Compose."
			>Inspire</button
		>
		<button
			type="button"
			disabled={busy}
			onclick={format}
			title="Step 1: LM inference to format existing lyrics for better generation quality. Next: Compose."
			>Format</button
		>
	</div>

	<details>
		<summary>Advanced LM</summary>
		<div class="details-body">
			<div class="meta-grid">
				<label
					>Temperature <input
						type="text"
						placeholder={ph(d?.lm_temperature)}
						bind:value={app.request.lm_temperature}
					/></label
				>
				<label
					>CFG scale <input
						type="text"
						placeholder={ph(d?.lm_cfg_scale)}
						bind:value={app.request.lm_cfg_scale}
					/></label
				>
				<label
					>Top P <input
						type="text"
						placeholder={ph(d?.lm_top_p)}
						bind:value={app.request.lm_top_p}
					/></label
				>
				<label
					>Top K <input
						type="text"
						placeholder={ph(d?.lm_top_k)}
						bind:value={app.request.lm_top_k}
					/></label
				>
			</div>
			<label
				>Negative prompt
				<textarea
					rows="4"
					placeholder="Styles or instruments to steer away from, e.g. saxophone, autotune, screaming, low quality..."
					bind:value={app.request.lm_negative_prompt}
				></textarea>
			</label>
			<label
				>Audio codes
				<textarea
					rows="4"
					placeholder="Filled by Compose. Do not edit unless you know what you are doing."
					bind:value={app.request.audio_codes}
				></textarea>
			</label>
		</div>
	</details>

	<div class="model-row">
		<span class="model-label">Batch</span>
		<input
			type="number"
			class="batch-input"
			min="1"
			max={app.props?.cli?.max_batch || 9}
			bind:value={app.request.lm_batch_size}
		/>
		<span class="spacer"></span>
		<span class="row-label">Pending</span>
		<div class="pending-nav">
			<button
				type="button"
				class="nav-btn"
				onclick={() => switchPending(-1)}
				title="Previous pending variation">&lt;</button
			>
			<span class="nav-label"
				>{app.pendingRequests.length > 0 ? app.pendingIndex + 1 : 0} / {app.pendingRequests
					.length}</span
			>
			<button
				type="button"
				class="nav-btn"
				onclick={() => switchPending(1)}
				title="Next pending variation">&gt;</button
			>
		</div>
	</div>

	<div class="action-row">
		<button
			type="button"
			disabled={busy}
			onclick={compose}
			title="Step 2: LM inference to generate audio codes that drive the flow matching. Next: Synthesize."
			>Compose</button
		>
		<button
			type="button"
			disabled={!busyLm}
			onclick={cancelPipeline}
			title="Cancel the active LM job">Cancel</button
		>
	</div>

	<details open>
		<summary>Task</summary>
		<div class="details-body">
			<div class="model-row">
				<span class="model-label">Type</span>
				<select
					value={taskType}
					onchange={(e) => {
						app.request.task_type = e.currentTarget.value;
					}}
				>
					<option value="">text2music</option>
					<option value={TASK_COVER}>cover</option>
					<option value={TASK_COVER_NOFSQ}>cover-nofsq</option>
					<option value={TASK_REPAINT}>repaint</option>
					<option value={TASK_LEGO}>lego</option>
					<option value={TASK_EXTRACT}>extract</option>
					<option value={TASK_COMPLETE}>complete</option>
				</select>
			</div>
			<div class="model-row track-row">
				<span class="model-label">Track</span>
				<div class="track-grid">
					{#each TRACK_NAMES as name}
						<button
							type="button"
							class="track-pill"
							class:active={selectedTracks.has(name)}
							disabled={!needsTrack}
							onclick={() => toggleTrack(name)}>{name}</button
						>
					{/each}
				</div>
			</div>
		</div>
	</details>

	<details open>
		<summary>Flow matching parameters</summary>
		<div class="details-body">
			<div class="meta-grid">
				<label
					>Steps <input
						type="text"
						placeholder={ph(dp?.inference_steps)}
						bind:value={app.request.inference_steps}
					/></label
				>
				<label
					>Cover strength <input
						type="text"
						placeholder={ph(d?.audio_cover_strength)}
						bind:value={app.request.audio_cover_strength}
					/></label
				>
				<label
					>Cover noise <input
						type="text"
						placeholder={ph(d?.cover_noise_strength)}
						bind:value={app.request.cover_noise_strength}
					/></label
				>
				<label
					>Repaint strength <input
						type="text"
						placeholder={ph(d?.repaint_strength)}
						bind:value={app.request.repaint_strength}
					/></label
				>
				<label
					>CFG scale <input
						type="text"
						placeholder={ph(dp?.guidance_scale)}
						bind:value={app.request.guidance_scale}
					/></label
				>
				<label
					>Shift <input
						type="text"
						placeholder={ph(dp?.shift)}
						bind:value={app.request.shift}
					/></label
				>
				<label
					>Method <select
						value={app.request.infer_method || ''}
						onchange={(e) => {
							app.request.infer_method = e.currentTarget.value;
						}}
					>
						<option value="">ODE Euler</option>
						<option value="sde">SDE Stochastic</option>
					</select></label
				>
				<label
					>Seed <input type="text" placeholder={ph(d?.seed)} bind:value={app.request.seed} /></label
				>
			</div>
		</div>
	</details>

	<div class="model-row">
		<span class="model-label">Batch</span>
		<input
			type="number"
			class="batch-input"
			min="1"
			max="9"
			bind:value={app.request.synth_batch_size}
		/>
		<span class="spacer"></span>
		<span class="row-label">Peak clip</span>
		<input
			type="number"
			class="peak-clip-input"
			min="0"
			max="999"
			bind:value={app.request.peak_clip}
		/>
		<label class="radio-label">
			<input type="radio" name="format" value="mp3" bind:group={app.format} /> MP3
		</label>
		<label class="radio-label">
			<input type="radio" name="format" value="wav" bind:group={app.format} /> WAV
		</label>
	</div>

	<div class="model-row cond-row">
		<span class="model-label">Cond</span>
		<div class="track-grid">
			<span class="dit-ind" class:on={hasCodes}>LM codes</span>
			<span class="dit-ind" class:on={hasSrc}>Src audio</span>
			<span class="dit-ind" class:on={hasRange}>Range</span>
			<span class="dit-ind" class:on={hasRef}>Timbre ref</span>
		</div>
	</div>

	<div class="action-row">
		<button
			type="button"
			disabled={busy}
			onclick={synthesize}
			title="Step 3: DiT flow matching + VAE decoding to synthesize audio from codes. No green indicators above (LM codes, Src audio, Timbre ref)? The DiT will hallucinate freely from your prompt: very creative, but unpredictable."
			>Synthesize</button
		>
		<button
			type="button"
			disabled={!busySynth}
			onclick={cancelPipeline}
			title="Cancel the active synth job">Cancel</button
		>
	</div>
</form>

<style>
	.request-form {
		display: flex;
		flex-direction: column;
		gap: 0.75rem;
	}
	.toolbar {
		display: flex;
		gap: 0.5rem;
	}
	.toolbar button {
		flex: 1;
		display: flex;
		align-items: center;
		justify-content: center;
		gap: 0.3rem;
	}
	label {
		display: flex;
		flex-direction: column;
		gap: 0.25rem;
		font-size: 0.85rem;
		color: var(--fg-dim);
	}
	.section-title {
		font-size: 0.85rem;
		color: var(--fg);
		font-weight: 600;
		padding: 0.4rem 0 0;
	}
	.lyrics-header {
		display: flex;
		align-items: center;
		justify-content: space-between;
	}
	.instrumental-toggle {
		display: flex;
		flex-direction: row;
		align-items: center;
		gap: 0.25rem;
		font-size: 0.8rem;
		font-weight: 400;
		color: var(--fg-dim);
		cursor: pointer;
	}
	.instrumental-toggle input[type='checkbox'] {
		cursor: pointer;
	}
	textarea,
	input[type='text'],
	input[type='number'],
	select {
		font-family: inherit;
		font-size: 0.9rem;
		padding: 0.4rem 0.5rem;
		border: 1px solid var(--border);
		border-radius: 4px;
		background: var(--bg-input);
		color: var(--fg);
		resize: vertical;
	}
	textarea:focus,
	input:focus {
		outline: 2px solid var(--focus);
		outline-offset: -1px;
	}
	.meta-grid {
		display: grid;
		grid-template-columns: repeat(auto-fill, minmax(8rem, 1fr));
		gap: 0.5rem;
	}
	details summary {
		cursor: pointer;
		font-size: 0.85rem;
		color: var(--fg);
		font-weight: 600;
		padding: 0.4rem 0;
	}
	details summary:hover {
		color: var(--fg);
	}
	.details-body {
		display: flex;
		flex-direction: column;
		gap: 0.5rem;
		padding: 0.25rem 0 0.5rem;
	}
	.model-row {
		display: flex;
		align-items: center;
		gap: 0.5rem;
	}
	.model-label {
		font-size: 0.85rem;
		color: var(--fg-dim);
		flex-shrink: 0;
		width: 2rem;
	}
	.model-row select {
		flex: 1;
		min-width: 0;
	}
	.spacer {
		flex: 1;
	}
	.row-label {
		font-size: 0.85rem;
		color: var(--fg-dim);
	}
	.radio-label {
		flex-direction: row;
		align-items: center;
		gap: 0.2rem;
		font-size: 0.85rem;
		color: var(--fg-dim);
		cursor: pointer;
	}
	.batch-input {
		padding: 0.2rem 0.3rem;
		font-size: 0.8rem;
		border: 1px solid var(--border);
		border-radius: 4px;
		background: var(--bg-input);
		color: var(--fg);
	}
	input.batch-input {
		width: 3rem;
		text-align: center;
	}
	input.peak-clip-input {
		width: 4rem;
		text-align: center;
	}
	.pending-nav {
		display: flex;
		align-items: center;
		gap: 0.4rem;
	}
	.nav-btn {
		padding: 0.4rem 0.4rem !important;
		font-size: 0.75rem !important;
		min-width: 0 !important;
	}
	.nav-label {
		font-size: 0.75rem;
		font-family: monospace;
		color: var(--fg);
	}
	button {
		padding: 0.5rem 1rem;
		border: 1px solid var(--border);
		border-radius: 4px;
		background: var(--bg-btn);
		color: var(--fg);
		cursor: pointer;
		font-size: 0.85rem;
	}
	button:hover:not(:disabled) {
		background: var(--bg-btn-hover);
	}
	button:disabled {
		opacity: 0.4;
	}
	.dit-ind {
		padding: 0.15rem 0.4rem;
		border-radius: 4px;
		font-size: 0.8rem;
		white-space: nowrap;
		background: var(--bg-err, #c0392b);
		color: #fff;
		opacity: 0.6;
		text-align: center;
		flex: 1;
	}
	.dit-ind.on {
		background: var(--bg-ok, #27ae60);
		opacity: 1;
	}
	.track-row,
	.cond-row {
		align-items: flex-start;
	}
	.track-row .model-label,
	.cond-row .model-label {
		padding-top: 0.2rem;
	}
	.track-grid {
		display: flex;
		flex-wrap: wrap;
		gap: 0.3rem;
		flex: 1;
	}
	.track-pill {
		padding: 0.2rem 0.5rem;
		border: 1px solid var(--border);
		border-radius: 4px;
		font-size: 0.8rem;
		font-family: inherit;
		cursor: pointer;
		background: var(--bg-input);
		color: var(--fg-dim);
		text-align: center;
		flex: 1;
		max-width: 33%;
	}
	.track-pill.active {
		background: var(--bg-btn-hover);
		color: var(--fg);
		border-color: var(--focus);
	}
	.lm-row {
		display: flex;
		gap: 0.5rem;
	}
	.lm-row button {
		flex: 1;
	}
	.action-row {
		display: flex;
		gap: 0.5rem;
	}
	.action-row button {
		flex: 1;
	}
</style>
