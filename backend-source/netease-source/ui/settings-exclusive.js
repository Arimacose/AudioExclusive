(() => {
  'use strict';
  const token = "__NCEXCLUSIVE_TOKEN__";
  if (window.__NCExclusiveDispose) window.__NCExclusiveDispose();
  let nextId = 0, disposed = false, scheduled = false, updating = false;
  let latest = null, row = null, observer = null, operationError = '';
  const pending = new Map();
  function request(operation, value = '0') {
    return new Promise((resolve, reject) => {
      const id = ++nextId;
      const timeout = setTimeout(() => finish(id, null, new Error('独占插件暂时无法连接')), 3000);
      pending.set(id, {resolve, reject, timeout});
      // CEF's native display-handler callback consumes the tagged console
      // message and replies into the token-bound frame. No navigation occurs.
      console.info(`__NCExclusiveRequest__/${token}/${id}/${operation}/${value}`);
    });
  }
  function finish(id, result, error) {
    const item = pending.get(id);
    if (!item) return;
    pending.delete(id); clearTimeout(item.timeout);
    if (error) item.reject(error); else item.resolve(result);
  }
  window.__NCExclusiveReply = (id, result) => finish(id, result, null);
  function anchorRow() {
    const elements = document.querySelectorAll('label,span,div,p,dt,dd,strong,b,h3,h4');
    for (const label of elements) {
      if (label.closest('[data-nc-exclusive]') || label.textContent.trim() !== '系统音频增强') continue;
      if ([...label.children].some(child => child.textContent.trim() === '系统音频增强')) continue;
      for (let candidate = label.parentElement, depth = 0; candidate && depth < 5; candidate = candidate.parentElement, depth++) {
        const bounds = candidate.getBoundingClientRect();
        if (bounds.height > 0 && bounds.height <= 120 && candidate.textContent.length < 600 &&
            (candidate.querySelector('input[type="radio"]') || candidate.textContent.includes('可能影响声音效果'))) return candidate;
      }
    }
    return null;
  }
  function message(state) {
    if (!state.supported) return '当前客户端版本暂不支持独占插件';
    if (state.pendingReopen) return '设置已保存，重新打开音频输出后生效';
    if (state.phase === 'faulted') {
      const errors = {
        0x8889000a: '输出设备正在被占用，无法开启独占',
        0x88890008: '输出设备不支持当前音频格式',
        0x8889000e: '系统未允许此设备使用独占模式',
        0x88890004: '输出设备已断开，请重新选择设备'
      };
      return errors[state.hresult] || '独占输出启动失败，请重新打开音频输出';
    }
    if (state.phase === 'active') return '独占输出已启用';
    if (!state.asio?.selected && state.wasapiOutput && !state.wasapiOutput.deviceOpen && state.enabled) return '设备已释放，播放时自动开启独占';
    if (state.phase === 'open') return '独占输出已打开，等待播放';
    if (state.enabled) return '已开启，等待音频输出启动';
    return '关闭时使用网易云原有输出模式';
  }
  function wasapiLabel() {
    // Read the selected control, never an item in its expanded device menu.
    // This also works before the first audio stream has been opened.
    for (const control of document.querySelectorAll('select,[role="combobox"]')) {
      if (control.closest('[data-nc-exclusive]')) continue;
      const value = (control.selectedOptions?.[0]?.textContent || control.getAttribute('aria-valuetext') || control.textContent || '').trim();
      const match = /^(Wasapi|DirectSound)\s*[:：]\s*(.+)$/i.exec(value);
      if (!match) continue;
      const name = match[2].trim();
      if (/^(默认输出设备|默认设备|系统默认|default(?: output)? device)$/i.test(name)) {
        return latest.wasapi?.defaultName ? `WASAPI：${latest.wasapi.defaultName}（系统默认）` : 'WASAPI（默认设备暂不可用）';
      }
      return `WASAPI：${name}`;
    }
    return latest.wasapi?.name ? `WASAPI：${latest.wasapi.name}` : 'WASAPI（未识别设备）';
  }
  function render() {
    if (!row || !latest) return;
    const input = row.querySelector('input');
    input.checked = latest.enabled;
    input.disabled = updating || !latest.supported;
    const select = row.querySelector('select');
    const drivers = latest.asio?.drivers || [];
    const signature = JSON.stringify(drivers);
    if (select.dataset.signature !== signature) {
      select.replaceChildren(new Option(wasapiLabel(), 'wasapi'));
      for (const driver of drivers) {
        const option = new Option(`ASIO: ${driver.name}${driver.compatible ? '' : '（需 64 位驱动）'}`, driver.id);
        option.disabled = !driver.compatible; option.title = driver.reason; select.append(option);
      }
      select.dataset.signature = signature;
    }
    const wasapi = select.options[0], label = wasapiLabel();
    if (wasapi.textContent !== label) wasapi.textContent = label;
    select.value = latest.asio?.selected || 'wasapi'; select.disabled = updating || !latest.supported;
    select.title = select.selectedOptions[0]?.textContent || label;
    const note = row.querySelector('[data-note]');
    const value = operationError || message(latest);
    if (note.textContent !== value) note.textContent = value;
  }
  function mount() {
    scheduled = false;
    if (disposed) return;
    if (row && !row.isConnected) row = null;
    if (row) {render(); return;}
    const anchor = anchorRow(); if (!anchor) return;
    row = document.createElement('div'); row.dataset.ncExclusive = 'true';
    row.style.cssText = 'display:flex;align-items:center;gap:16px;flex-wrap:wrap;min-height:42px;margin:0;color:inherit;font:inherit;';
    row.style.fontSize = getComputedStyle(anchor).fontSize;
    const heading = document.createElement('span'); heading.textContent = '独占输出';
    const nativeLabel = [...anchor.querySelectorAll('label,span,div,p,dt')].find(node => node.textContent.trim() === '系统音频增强');
    if (nativeLabel) {
      const width = Math.max(88, Math.min(180, nativeLabel.getBoundingClientRect().width));
      heading.style.cssText = `flex:0 0 ${width}px;font-weight:500;`;
    } else heading.style.cssText = 'flex:0 0 100px;font-weight:500;';
    const label = document.createElement('label');
    label.style.cssText = 'display:inline-flex;align-items:center;gap:6px;cursor:pointer;';
    const input = document.createElement('input'); input.type = 'checkbox'; input.disabled = true;
    input.style.accentColor = '#ff3a3a'; label.append(input, document.createTextNode('开启独占'));
    const note = document.createElement('span'); note.dataset.note = 'true';
    note.style.cssText = 'opacity:.7;font-size:12px;'; note.setAttribute('role', 'status');
    const select = document.createElement('select'); select.setAttribute('aria-label', '独占输出后端');
    select.style.cssText = 'max-width:300px;color:inherit;background:transparent;border:1px solid #aaa;border-radius:5px;padding:3px;';
    select.addEventListener('change', async () => {
      const wanted = select.value; updating = true; operationError = ''; render();
      try {
        const result = await request('output', wanted); latest = result.state;
        if (!result.ok) throw new Error('驱动不可用或设置保存失败');
        operationError = '输出后端已保存，重新打开音频输出后生效';
      } catch (error) { operationError = error.message; }
      finally { updating = false; render(); }
    });
    note.textContent = '正在连接独占插件'; row.append(heading, label, select, note); anchor.insertAdjacentElement('afterend', row);
    input.addEventListener('change', async () => {
      const wanted = input.checked; updating = true; operationError = ''; render();
      try {
        const result = await request('set', wanted ? '1' : '0');
        latest = result.state;
        if (!result.ok) throw new Error('设置未能保存，请检查插件状态');
      } catch (error) {operationError = error.message; note.textContent = operationError;}
      finally {updating = false; render();}
    });
    render(); request('mounted', '1').then(poll).catch(() => {});
  }
  function schedule() {if (!scheduled && !disposed) {scheduled = true; requestAnimationFrame(mount);}}
  let pollInFlight = false;
  async function poll() {
    if (disposed || pollInFlight || !row || !row.isConnected || updating) return;
    pollInFlight = true;
    try {const result = await request('status'); latest = result.state; render();}
    catch (error) {if (row) {row.querySelector('[data-note]').textContent = error.message;row.querySelector('input').disabled = true;}}
    finally {pollInFlight = false;}
  }
  observer = new MutationObserver(schedule);
  observer.observe(document.documentElement, {subtree:true, childList:true});
  const timer = setInterval(poll, 1000);
  const dispose = () => {
    if (disposed) return;
    disposed = true;
    window.removeEventListener('pagehide', dispose);
    observer.disconnect(); clearInterval(timer);
    for (const id of [...pending.keys()]) finish(id, null, new Error('页面已关闭'));
    if (row) row.remove();
    row = null; latest = null; observer = null;
    if (window.__NCExclusiveDispose === dispose) {
      delete window.__NCExclusiveReply; delete window.__NCExclusiveDispose;
    }
  };
  window.__NCExclusiveDispose = dispose;
  window.addEventListener('pagehide', dispose, {once:true});
  request('boot', '1').catch(() => {});
  schedule();
})();
