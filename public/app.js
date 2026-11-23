'use strict';
const $ = id => document.getElementById(id);
const money = number => new Intl.NumberFormat('en-IN', {style:'currency', currency:'INR', minimumFractionDigits:2}).format(number);
let connected = false;
const audit = $('audit');

async function api(path, data) {
  const options = {credentials:'same-origin'};
  if (data) {
    options.method = 'POST';
    options.headers = {'Content-Type':'application/x-www-form-urlencoded', 'X-Bank-Request':'1'};
    options.body = new URLSearchParams(data);
  }
  const response = await fetch(path, options);
  let result;
  try { result = await response.json(); }
  catch { throw new Error('The C backend did not return a valid response. Try again shortly.'); }
  if (!response.ok || !result.ok) throw new Error(result.error || 'The operation could not be completed.');
  return result;
}
function render(data) {
  for (const account of data.accounts) $('balance-' + account.id).textContent = money(account.balance);
  $('total').textContent = money(data.total);
}
function message(id, text, status = '') { $(id).textContent = text; $(id).className = 'message ' + status; }
function log(text) {
  if (!text) return;
  if (audit.textContent.startsWith('Ready.') || audit.textContent === 'No transactions in this view.') audit.textContent = '';
  audit.textContent += '[' + new Date().toLocaleTimeString('en-IN') + ']\n' + text + '\n';
  if (audit.textContent.length > 65000) audit.textContent = audit.textContent.slice(-60000);
  audit.scrollTop = audit.scrollHeight;
}
function busy(button, value) { button.disabled = value || !connected; }
function operationFields() {
  const operation = $('operation').value;
  $('target-field').hidden = operation !== 'transfer';
  $('amount-field').hidden = operation === 'enquiry';
  $('amount').required = operation !== 'enquiry';
  if ($('target').value === $('account').value) $('target').value = $('account').value === '101' ? '102' : '101';
}
$('operation').addEventListener('change', operationFields);
$('account').addEventListener('change', operationFields);
$('transaction-form').addEventListener('submit', async event => {
  event.preventDefault();
  const data = Object.fromEntries(new FormData(event.target));
  if (data.operation === 'transfer' && data.account === data.target) {
    message('transaction-status', 'Choose two different accounts for a transfer.', 'error'); return;
  }
  busy($('submit'), true);
  message('transaction-status', 'Processing on the C backend…');
  try {
    const result = await api('/api/transaction', data); render(result); log(result.log);
    const rejected = /REJECTED|FAILED/.test(result.log);
    message('transaction-status', rejected ? 'Transaction rejected: insufficient funds.' : 'Transaction completed.', rejected ? 'error' : 'success');
  } catch(error) { message('transaction-status', error.message, 'error'); }
  finally { busy($('submit'), false); }
});
$('refresh').addEventListener('click', async () => {
  busy($('refresh'), true);
  try { render(await api('/api/state')); message('transaction-status', 'Balances refreshed.', 'success'); }
  catch(error) { message('transaction-status', error.message, 'error'); }
  finally { busy($('refresh'), false); }
});
$('reset').addEventListener('click', async () => {
  busy($('reset'), true);
  try { const result = await api('/api/reset', {}); render(result); log(result.log); message('transaction-status', 'Both demo accounts reset to ₹50,000.', 'success'); }
  catch(error) { message('transaction-status', error.message, 'error'); }
  finally { busy($('reset'), false); }
});
for (const button of document.querySelectorAll('[data-demo]')) {
  button.addEventListener('click', async () => {
    const buttons = [...document.querySelectorAll('[data-demo]')]; buttons.forEach(b => b.disabled = true);
    message('demo-status', 'Running concurrent C workers…');
    try {
      const result = await api('/api/demo', {kind:button.dataset.demo}); log(result.log);
      if (result.kind === 'race') {
        message('demo-status', result.actual < result.expected
          ? `Race observed. Expected ${money(result.expected)}; actual ${money(result.actual)}. Lost ${money(result.expected - result.actual)}.`
          : 'No lost update in this run. Scheduling varies; run it again.');
      } else if (result.kind === 'assignment') {
        message('demo-status', `Assignment completed. Account 101: ${money(result.account101)}; account 102: ${money(result.account102)}. See the audit for scheduling outcomes.`, 'success');
      } else {
        const passed = Math.abs(result.expected - result.actual) < 0.005;
        message('demo-status', `${passed ? 'PASS' : 'FAIL'} · Expected ${money(result.expected)}; actual ${money(result.actual)}. ${result.kind === 'transfer' ? 'All 100 workers joined.' : 'All 10 deposits preserved.'}`, passed ? 'success' : 'error');
      }
    } catch(error) { message('demo-status', error.message, 'error'); }
    finally { buttons.forEach(b => b.disabled = !connected); }
  });
}
$('clear').addEventListener('click', () => { audit.textContent = 'No transactions in this view.'; });
(async function init() {
  try {
    const data = await api('/api/state'); render(data); connected = true;
    $('connection').textContent = 'C backend connected'; $('connection').className = 'connection connected';
    $('submit').disabled = false; $('reset').disabled = false;
    document.querySelectorAll('[data-demo]').forEach(button => button.disabled = false);
    message('transaction-status', 'Ready to make a transaction.');
  } catch(error) {
    $('connection').textContent = 'Backend unavailable'; $('connection').className = 'connection failed';
    message('transaction-status', error.message + ' Reload this page to reconnect.', 'error');
  }
  operationFields();
})();
