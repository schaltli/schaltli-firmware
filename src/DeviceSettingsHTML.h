#pragma once

// The device-settings form, shared verbatim by the two web servers that
// serve it - WiFiSetupServer's tabbed config page (setup mode) and
// TestInterfaceServer's /settings (normal operation).
//
// One fragment rather than one page each, because the two servers own port
// 80 at different times and neither is reachable when the other is: the
// config page only exists in setup mode, and setup mode stops the test
// interface. A setting that lives in only one of them is therefore a
// setting you can reach only by putting the device into the wrong state to
// change it. Copying the form into both would work exactly until the two
// copies drifted, which is the same trap the ported "M5 Dial" strings fell
// into - so it is one string, substituted into both shells.
//
// Markup only, no <style>: the class names (form-group, hint, btn,
// message) are the config page's own, and /settings defines the same ones.
// Both shells therefore render this identically, which is the point - a
// person changing a timeout should not be able to tell which of the two
// pages they are on.

static const char DEVICE_SETTINGS_TAB_HTML[] PROGMEM = R"=====(
<div id="device-message" class="message"></div>
<div class="form-group">
<label for="dsOff">Turn the display off after</label>
<input id="dsOff" type="number" min="0" max="86400" step="1" placeholder="15">
<div class="hint">Seconds without a touch or a turn of the knob. 0 keeps the display
on for good. Values arriving over MQTT do not count as activity, so a screen showing
live readings still goes dark.</div>
</div>
<div class="form-group">
<label for="dsIdle">When it goes dark, return to</label>
<select id="dsIdle"><option value="">Stay on the current screen</option></select>
<div class="hint">This screen is also the one shown at boot. Point it at something
harmless - a light, say - so the panel is never found sitting on a screen where a
mistaken touch matters. The critical screens stay one swipe away.</div>
<div id="dsNoProject" class="hint" style="display:none;color:#c62828">No project is
loaded right now, so the screen list is empty. Any screen already chosen is kept.</div>
</div>
<button class="btn" onclick="deviceSettingsSave()">Save Device Settings</button>
)=====";

static const char DEVICE_SETTINGS_JS[] PROGMEM = R"=====(
function deviceSettingsMessage(type,text){
var m=document.getElementById('device-message');
m.className='message show '+type;
m.textContent=text;
setTimeout(function(){m.classList.remove('show')},5000);}
function deviceSettingsLoad(){
fetch('/api/device-settings').then(function(r){return r.json()}).then(function(d){
document.getElementById('dsOff').value=d.displayOffAfterSeconds;
var sel=document.getElementById('dsIdle');
sel.innerHTML='';
var none=document.createElement('option');
none.value='';
none.textContent='Stay on the current screen';
sel.appendChild(none);
var screens=d.screens||[];
var found=false;
screens.forEach(function(s){
var o=document.createElement('option');
o.value=s.id;
o.textContent=s.name?s.name:s.id;
sel.appendChild(o);
if(s.id===d.idleScreenId){found=true}});
// A saved id belonging to a project that is not loaded right now (setup mode
// reached from a cold boot) must stay selected rather than being quietly
// reset to "none" by a dropdown that cannot see it.
if(d.idleScreenId&&!found){
var o=document.createElement('option');
o.value=d.idleScreenId;
o.textContent=d.idleScreenId+' (not in the installed project)';
sel.appendChild(o);}
sel.value=d.idleScreenId?d.idleScreenId:'';
document.getElementById('dsNoProject').style.display=screens.length?'none':'block';
}).catch(function(){
deviceSettingsMessage('error','Could not read the current settings')});}
function deviceSettingsSave(){
var f=new FormData();
f.append('displayOffAfterSeconds',document.getElementById('dsOff').value);
f.append('idleScreenId',document.getElementById('dsIdle').value);
fetch('/api/device-settings',{method:'POST',body:f})
.then(function(r){return r.json()})
.then(function(d){deviceSettingsMessage(d.success?'success':'error',d.message)})
.catch(function(){deviceSettingsMessage('error','Could not reach the device')});}
window.addEventListener('load',deviceSettingsLoad);
)=====";
