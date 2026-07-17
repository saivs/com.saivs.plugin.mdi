// MDIBackend_WebGPU.jslib — Multi-Draw Indirect backend for Unity Web builds
// running the WebGPU graphics API.
//
// Unity exposes no IUnityGraphicsWebGPU plugin interface, so this backend works
// the way the Metal backend does — by intercepting the graphics API at the
// point Unity itself calls it. Unity's WebGPU backend drives the browser API
// through lib_webgpu.js, and every command ultimately invokes a method on a JS
// prototype (GPURenderPassEncoder etc.). We patch those prototypes at module
// load, before the engine creates its device.
//
// Flow (mirrors the native backends):
//  1. C# records a zero-instance "prime" draw with the dummy args buffer at
//     argsOffset = slot*20. Unity encodes it as
//     drawIndexedIndirect(dummyBuffer, slot*20) with the full pipeline state
//     already bound on the pass encoder.
//  2. The drawIndexedIndirect patch recognizes the dummy buffer, reads
//     MDIParams from the pinned C# ring buffer (HEAPU32) and replaces the
//     prime with the real multi-draw:
//       a. encoder.multiDrawIndexedIndirect(...)   — when the (experimental)
//          'chromium-experimental-multi-draw-indirect' feature is present;
//       b. a cached GPURenderBundle with N drawIndexedIndirect commands,
//          replayed with a single executeBundles() call (standard WebGPU);
//       c. a plain loop of N drawIndexedIndirect calls — if bundle
//          validation ever fails.
//  3. Pass state (pipeline, bind groups, index/vertex buffers) is
//     shadow-tracked from Unity's own calls so bundles can re-bind it, and is
//     re-applied after executeBundles(), which resets render pass state.
//
// All WebGPU descriptor keys and method calls use quoted/bracket syntax to
// survive Closure Compiler minification, same as Unity's lib_webgpu.js.

var LibraryMDIWebGPU = {

  $MDIWGPU__postset: 'MDIWGPU.install();',
  $MDIWGPU: {
    MAX_PENDING: 256,
    ARGS_STRIDE: 20,          // sizeof(IndirectDrawIndexedArgs)
    RING_STRIDE_U32: 8,       // sizeof(NativeMDIParams) on wasm32 = 32 bytes
    MAGIC: 0x4D444921,        // must match MDI_RING_MAGIC in MultiDrawIndirect.cs

    device: null,
    hasMultiDraw: false,
    hasFirstInstance: false,
    dummyBuffer: null,        // GPUBuffer of the C# dummy args buffer
    ringPtr: 0,               // byte address of the pinned NativeMDIParams ring
    slotCounter: 0,
    loopMode: false,          // set after a bundle failure — loop from then on
    bundleBuilds: 0,          // diagnostics: bundles built since startup

    viewInfo: null,           // WeakMap: GPUTextureView -> {fmt, samples}
    passInfo: null,           // WeakMap: GPURenderPassEncoder -> {bundleDesc, fmtKey, state}
    objIds: null,             // WeakMap: any object -> unique int (cache keys)
    nextObjId: 1,
    bundleCache: null,        // Map: key string -> GPURenderBundle
    orig: {},                 // original prototype methods

    // ---------------------------------------------------------------------
    table: function() {
      // lib_webgpu.js handle table (int handle -> JS object). Present in every
      // Unity WebGPU build; absent in plain WebGL2 builds.
      return (typeof wgpu !== 'undefined') ? wgpu : null;
    },

    idOf: function(o) {
      var m = MDIWGPU.objIds, id = m.get(o);
      if (!id) { id = MDIWGPU.nextObjId++; m.set(o, id); }
      return id;
    },

    log: function(msg) { console.log('[MDI][WebGPU] ' + msg); },
    warnOnce: {},
    warn: function(key, msg) {
      if (MDIWGPU.warnOnce[key]) return;
      MDIWGPU.warnOnce[key] = 1;
      console.warn('[MDI][WebGPU] ' + msg);
    },

    onDevice: function(dev) {
      MDIWGPU.device = dev;
      MDIWGPU.hasFirstInstance = dev['features'].has('indirect-first-instance');
      MDIWGPU.hasMultiDraw = dev['features'].has('chromium-experimental-multi-draw-indirect');
      MDIWGPU.bundleCache = new Map();
      MDIWGPU.loopMode = false;
      MDIWGPU.log('device captured, indirect-first-instance=' + MDIWGPU.hasFirstInstance +
                  ', multiDrawIndirect=' + MDIWGPU.hasMultiDraw);
    },

    // ---------------------------------------------------------------------
    // Prototype patches
    // ---------------------------------------------------------------------
    install: function() {
      if (typeof GPUAdapter === 'undefined' || typeof GPURenderPassEncoder === 'undefined')
        return; // browser has no WebGPU — MDI_IsSupported() will report 0

      MDIWGPU.viewInfo = new WeakMap();
      MDIWGPU.passInfo = new WeakMap();
      MDIWGPU.objIds   = new WeakMap();
      MDIWGPU.bundleCache = new Map();
      var orig = MDIWGPU.orig;

      // --- device creation: append optional features Unity doesn't request
      orig.requestDevice = GPUAdapter.prototype['requestDevice'];
      GPUAdapter.prototype['requestDevice'] = function(desc) {
        try {
          desc = desc || {};
          var req = desc['requiredFeatures'] ? Array.from(desc['requiredFeatures']) : [];
          var feats = this['features'];
          ['indirect-first-instance', 'chromium-experimental-multi-draw-indirect']
            .forEach(function(f) {
              if (feats && feats.has(f) && req.indexOf(f) < 0) req.push(f);
            });
          desc = Object.assign({}, desc);
          desc['requiredFeatures'] = req;
        } catch (e) { /* never break device creation */ }
        var promise = orig.requestDevice.call(this, desc);
        promise.then(function(dev) { if (dev) MDIWGPU.onDevice(dev); })
               .catch(function() {});
        return promise;
      };

      // --- texture views: remember format/sampleCount (views expose neither)
      orig.createView = GPUTexture.prototype['createView'];
      GPUTexture.prototype['createView'] = function(vdesc) {
        var view = orig.createView.call(this, vdesc);
        try {
          MDIWGPU.viewInfo.set(view, {
            fmt: (vdesc && vdesc['format']) || this['format'],
            samples: this['sampleCount']
          });
        } catch (e) {}
        return view;
      };

      // --- render pass begin: capture attachment formats for bundle descriptors
      orig.beginRenderPass = GPUCommandEncoder.prototype['beginRenderPass'];
      GPUCommandEncoder.prototype['beginRenderPass'] = function(rdesc) {
        var pass = orig.beginRenderPass.call(this, rdesc);
        try {
          var colorFormats = [], samples = 1, depthFmt = undefined;
          var depthRO = false, stencilRO = false;
          var atts = rdesc['colorAttachments'] || [];
          for (var i = 0; i < atts.length; i++) {
            var a = atts[i];
            if (!a) { colorFormats.push(null); continue; }
            var vi = MDIWGPU.viewInfo.get(a['view']);
            colorFormats.push(vi ? vi.fmt : null);
            if (vi) samples = vi.samples;
          }
          var dsa = rdesc['depthStencilAttachment'];
          if (dsa) {
            var dvi = MDIWGPU.viewInfo.get(dsa['view']);
            if (dvi) { depthFmt = dvi.fmt; samples = dvi.samples; }
            depthRO = !!dsa['depthReadOnly'];
            stencilRO = !!dsa['stencilReadOnly'];
          }
          var bundleDesc = {
            'colorFormats': colorFormats,
            'sampleCount': samples,
            'depthReadOnly': depthRO,
            'stencilReadOnly': stencilRO
          };
          if (depthFmt) bundleDesc['depthStencilFormat'] = depthFmt;
          MDIWGPU.passInfo.set(pass, {
            bundleDesc: bundleDesc,
            fmtKey: colorFormats.join(',') + '/' + depthFmt + '/' + samples + '/' + depthRO + stencilRO,
            state: { pipeline: null, bindGroups: [], ib: null, vbs: [] }
          });
        } catch (e) {}
        return pass;
      };

      // --- shadow state tracking
      var rp = GPURenderPassEncoder.prototype;
      var st = function(enc) {
        var pi = MDIWGPU.passInfo.get(enc);
        return pi ? pi.state : null;
      };

      orig.setPipeline = rp['setPipeline'];
      rp['setPipeline'] = function(p) {
        var s = st(this); if (s) s.pipeline = p;
        return orig.setPipeline.call(this, p);
      };

      orig.setBindGroup = rp['setBindGroup'];
      rp['setBindGroup'] = function(index, bg, a, b, c) {
        var s = st(this);
        if (s) {
          var offs = null;
          if (a) {
            offs = ArrayBuffer.isView(a)
              ? Array.prototype.slice.call(a, b | 0, (b | 0) + (c | 0))
              : Array.prototype.slice.call(a);
          }
          s.bindGroups[index] = { bg: bg, offs: (offs && offs.length) ? offs : null };
        }
        return orig.setBindGroup.apply(this, arguments);
      };

      orig.setIndexBuffer = rp['setIndexBuffer'];
      rp['setIndexBuffer'] = function(buffer, format, offset, size) {
        var s = st(this); if (s) s.ib = { b: buffer, f: format, o: offset || 0, s: size };
        return orig.setIndexBuffer.apply(this, arguments);
      };

      orig.setVertexBuffer = rp['setVertexBuffer'];
      rp['setVertexBuffer'] = function(slot, buffer, offset, size) {
        var s = st(this); if (s) s.vbs[slot] = { b: buffer, o: offset || 0, s: size };
        return orig.setVertexBuffer.apply(this, arguments);
      };

      // --- the interception point
      orig.drawIndexedIndirect = rp['drawIndexedIndirect'];
      rp['drawIndexedIndirect'] = function(buffer, offset) {
        if (MDIWGPU.isPrime(buffer, offset)) {
          if (MDIWGPU.executeMDI(this, offset)) return;
        }
        return orig.drawIndexedIndirect.call(this, buffer, offset);
      };

      MDIWGPU.log('prototype patches installed');
    },

    // ---------------------------------------------------------------------
    // Prime draw detection
    // ---------------------------------------------------------------------
    isPrime: function(buffer, offset) {
      if (!MDIWGPU.ringPtr) return false;

      if (MDIWGPU.dummyBuffer)
        return buffer === MDIWGPU.dummyBuffer && MDIWGPU.slotValid(offset);

      // Learning mode — GetNativeBufferPtr() didn't resolve, identify the dummy
      // buffer from its unique shape: MAX_PENDING*20 bytes, INDIRECT usage, and
      // a live magic-tagged ring slot at this offset.
      if (buffer['size'] !== MDIWGPU.MAX_PENDING * MDIWGPU.ARGS_STRIDE) return false;
      if (!(buffer['usage'] & 0x100 /* GPUBufferUsage.INDIRECT */)) return false;
      if (!MDIWGPU.slotValid(offset)) return false;
      MDIWGPU.dummyBuffer = buffer;
      MDIWGPU.log('dummy args buffer identified heuristically');
      return true;
    },

    slotValid: function(offset) {
      if (offset % MDIWGPU.ARGS_STRIDE) return false;
      var slot = (offset / MDIWGPU.ARGS_STRIDE) | 0;
      if (slot >= MDIWGPU.MAX_PENDING) return false;
      var base = (MDIWGPU.ringPtr >>> 2) + slot * MDIWGPU.RING_STRIDE_U32;
      return HEAPU32[base + 7] === MDIWGPU.MAGIC &&  // _pad carries the magic
             HEAPU32[base + 3] > 0 &&                // maxDrawCount
             HEAPU32[base] !== 0;                    // argsBuffer handle
    },

    // ---------------------------------------------------------------------
    // MDI execution: multi-draw -> render bundle -> loop
    // ---------------------------------------------------------------------
    executeMDI: function(encoder, offset) {
      var slot = (offset / MDIWGPU.ARGS_STRIDE) | 0;
      var base = (MDIWGPU.ringPtr >>> 2) + slot * MDIWGPU.RING_STRIDE_U32;
      var argsHandle = HEAPU32[base];
      var argsOffset = HEAPU32[base + 2];
      var count      = HEAPU32[base + 3];

      var t = MDIWGPU.table();
      var argsBuf = t ? t[argsHandle] : null;
      if (!(typeof GPUBuffer !== 'undefined' && argsBuf instanceof GPUBuffer)) {
        MDIWGPU.warn('args', 'args buffer handle ' + argsHandle +
          ' did not resolve to a GPUBuffer — executing prime draw as-is (no MDI)');
        return false;
      }

      if (MDIWGPU.hasMultiDraw) {
        encoder['multiDrawIndexedIndirect'](argsBuf, argsOffset, count);
        return true;
      }

      if (!MDIWGPU.loopMode) {
        try {
          MDIWGPU.executeBundle(encoder, argsBuf, argsOffset, count);
          return true;
        } catch (e) {
          MDIWGPU.warn('bundle', 'render bundle path failed (' + e +
            ') — switching to drawIndexedIndirect loop');
          MDIWGPU.loopMode = true;
        }
      }

      for (var i = 0; i < count; i++)
        MDIWGPU.orig.drawIndexedIndirect.call(
          encoder, argsBuf, argsOffset + i * MDIWGPU.ARGS_STRIDE);
      return true;
    },

    executeBundle: function(encoder, argsBuf, argsOffset, count) {
      var pi = MDIWGPU.passInfo.get(encoder);
      var s = pi && pi.state;
      if (!s || !s.pipeline || !s.ib)
        throw 'incomplete tracked pass state';

      // Cache key: everything the bundle bakes in.
      var key = pi.fmtKey + '|' + MDIWGPU.idOf(s.pipeline);
      for (var i = 0; i < s.bindGroups.length; i++) {
        var g = s.bindGroups[i];
        key += '|' + (g ? (MDIWGPU.idOf(g.bg) + (g.offs ? ':' + g.offs.join(',') : '')) : '');
      }
      key += '|i' + MDIWGPU.idOf(s.ib.b) + ':' + s.ib.f + ':' + s.ib.o + ':' + s.ib.s;
      for (i = 0; i < s.vbs.length; i++) {
        var v = s.vbs[i];
        key += '|v' + (v ? (MDIWGPU.idOf(v.b) + ':' + v.o + ':' + v.s) : '');
      }
      key += '|a' + MDIWGPU.idOf(argsBuf) + ':' + argsOffset + ':' + count;

      var bundle = MDIWGPU.bundleCache.get(key);
      if (!bundle) {
        if (MDIWGPU.bundleCache.size > 512) MDIWGPU.bundleCache.clear();
        var be = MDIWGPU.device['createRenderBundleEncoder'](pi.bundleDesc);
        be['setPipeline'](s.pipeline);
        for (i = 0; i < s.bindGroups.length; i++) {
          g = s.bindGroups[i];
          if (!g) continue;
          if (g.offs) be['setBindGroup'](i, g.bg, g.offs);
          else be['setBindGroup'](i, g.bg);
        }
        be['setIndexBuffer'](s.ib.b, s.ib.f, s.ib.o, s.ib.s);
        for (i = 0; i < s.vbs.length; i++) {
          v = s.vbs[i];
          if (v) be['setVertexBuffer'](i, v.b, v.o, v.s);
        }
        for (i = 0; i < count; i++)
          be['drawIndexedIndirect'](argsBuf, argsOffset + i * MDIWGPU.ARGS_STRIDE);
        bundle = be['finish']();
        MDIWGPU.bundleCache.set(key, bundle);
        if (++MDIWGPU.bundleBuilds % 256 === 0)
          MDIWGPU.warn('churn' + MDIWGPU.bundleBuilds,
            MDIWGPU.bundleBuilds + ' bundles built — check for per-frame state churn');
      }

      encoder['executeBundles']([bundle]);

      // executeBundles() resets pass state — restore what Unity had bound so
      // its subsequent draws in this pass are unaffected. Original methods:
      // tracked state stays accurate.
      var o = MDIWGPU.orig;
      o.setPipeline.call(encoder, s.pipeline);
      for (i = 0; i < s.bindGroups.length; i++) {
        g = s.bindGroups[i];
        if (!g) continue;
        if (g.offs) o.setBindGroup.call(encoder, i, g.bg, g.offs);
        else o.setBindGroup.call(encoder, i, g.bg);
      }
      o.setIndexBuffer.call(encoder, s.ib.b, s.ib.f, s.ib.o, s.ib.s);
      for (i = 0; i < s.vbs.length; i++) {
        v = s.vbs[i];
        if (v) o.setVertexBuffer.call(encoder, i, v.b, v.o, v.s);
      }
    }
  },

  // -----------------------------------------------------------------------
  // C API — mirrors the native GfxPluginMDI exports. On WebGL2 builds (no
  // WebGPU) everything degrades to "unsupported" and the C# side falls back
  // to its DrawProceduralIndirect loop.
  // -----------------------------------------------------------------------
  MDI_IsSupported: function() {
    // indirect-first-instance is required: draw args rely on startInstance,
    // and without the feature such draws are silently discarded.
    return (MDIWGPU.device && MDIWGPU.hasFirstInstance) ? 1 : 0;
  },

  MDI_AllocSlot: function() {
    var slot = MDIWGPU.slotCounter % MDIWGPU.MAX_PENDING;
    MDIWGPU.slotCounter = (MDIWGPU.slotCounter + 1) % MDIWGPU.MAX_PENDING;
    return slot;
  },

  MDI_GetBaseEventID: function() { return 0; },

  // No native render-event callback on the Web — interception happens inside
  // the prime draw itself. C# skips IssuePluginEventAndData when this is 0.
  MDI_GetRenderEventAndDataFunc: function() { return 0; },

  MDI_SetDummyArgsBuffer: function(ptr) {
    var t = MDIWGPU.table();
    var buf = (t && ptr) ? t[ptr] : null;
    if (typeof GPUBuffer !== 'undefined' && buf instanceof GPUBuffer) {
      MDIWGPU.dummyBuffer = buf;
      MDIWGPU.log('dummy args buffer resolved from native handle ' + ptr);
    } else {
      MDIWGPU.dummyBuffer = null; // heuristic identification on first prime
      MDIWGPU.log('native handle ' + ptr +
        ' did not resolve to a GPUBuffer — will identify the dummy buffer heuristically');
    }
  },

  MDI_SetParamsRing: function(ptr) { MDIWGPU.ringPtr = ptr; },

  MDI_SetDrawIndexBuffer: function(ptr) {},        // Metal-only concept
  MDI_UsesPerInstanceVB: function() { return 0; }, // instance ID via instance_index
  MDI_SetMaxInstanceCount: function(maxCount) { return 1; },
  MDI_GetMaxInstanceCount: function() { return 0; },
  MDI_SetLogCallback: function(cb) {},             // JS logs go to the console
};

autoAddDeps(LibraryMDIWebGPU, '$MDIWGPU');
mergeInto(LibraryManager.library, LibraryMDIWebGPU);
