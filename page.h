#pragma once
#include <pgmspace.h>

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Salah</title>
<link rel=stylesheet href=https://unpkg.com/leaflet@1.9.4/dist/leaflet.css>
<style>
body{margin:0;font:15px system-ui,sans-serif;background:#111;color:#eee}
#map{height:45vh}
.p{padding:12px;max-width:480px;margin:auto}
label{display:block;margin:8px 0 2px;color:#aaa;font-size:12px}
input,select,button{width:100%;box-sizing:border-box;padding:8px;font-size:15px;background:#222;color:#eee;border:1px solid #444;border-radius:6px}
.r{display:flex;gap:8px}
button{background:#c0392b;border:0;margin-top:12px}
table{width:100%;border-collapse:collapse;margin-top:14px}
td,th{padding:6px;text-align:left;border-bottom:1px solid #333}
tr.c td{color:#ff6b57;font-weight:700}
</style></head><body>
<div id=map></div>
<div class=p>
<div class=r><div><label>Latitude</label><input id=lat></div><div><label>Longitude</label><input id=lon></div></div>
<div class=r><div><label>UTC offset (hours)</label><input id=tz type=number step=0.25></div>
<div><label>Asr</label><select id=a><option value=0>Standard<option value=1>Hanafi</select></div></div>
<label>Region / method</label>
<select id=m><option value=1>Karachi (South Asia)<option value=2>ISNA (North America)<option value=3>Muslim World League<option value=4>Umm al-Qura (Makkah)<option value=5>Egyptian Authority</select>
<button onclick=save()>Save &amp; update display</button>
<label>Wi-Fi network</label><input id=ws autocomplete=off><label>Wi-Fi password</label><input id=wp type=password autocomplete=off>
<button onclick=wifi()>Save Wi-Fi &amp; restart</button>
<div id=d style="margin-top:14px;color:#aaa"></div>
<table><tr><th><th>Start<th>End</tr><tbody id=t></tbody></table>
</div>
<script src=https://unpkg.com/leaflet@1.9.4/dist/leaflet.js></script>
<script>
const N=['Fajr','Dhuhr','Asr','Maghrib','Isha'],$=i=>document.getElementById(i);
let map,mk;
function pos(p){$('lat').value=p.lat.toFixed(5);$('lon').value=p.lng.toFixed(5)}
function show(s,first){
  if(first&&!s.set)s.tz=-new Date().getTimezoneOffset()*1;
  pos({lat:s.lat,lng:s.lon});
  $('tz').value=s.tz/60;$('m').value=s.m;$('a').value=s.a;
  $('ws').value=s.ssid||'';
  $('d').textContent=s.ap?'No internet: enter coordinates, save, then set Wi-Fi':s.date+' | now '+s.now;
  $('t').innerHTML=s.t.map((x,i)=>`<tr${i==s.cur?' class=c':''}><td>${N[i]}<td>${x[0]}<td>${x[1]}`).join('');
}
function save(){
  const b=document.querySelector('button');b.textContent='Fetching times...';b.disabled=true;
  fetch('/api/save?lat='+$('lat').value+'&lon='+$('lon').value+'&tz='+Math.round($('tz').value*60)+'&m='+$('m').value+'&a='+$('a').value)
    .then(r=>r.ok?r.json():Promise.reject()).then(s=>show(s,false)).catch(()=>alert('Could not fetch times')).finally(()=>{b.textContent='Save & update display';b.disabled=false});
}
function wifi(){
  if(!$('ws').value)return alert('Enter network name');
  fetch('/api/wifi?ssid='+encodeURIComponent($('ws').value)+'&pass='+encodeURIComponent($('wp').value)).then(()=>alert('Saved. Device is restarting.')).catch(()=>alert('Failed'));
}
fetch('/api/state').then(r=>r.json()).then(s=>{
  show(s,true);
  if(!window.L){$('map').style.display='none';return}
  map=L.map('map').setView([s.lat,s.lon],10);
  L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{attribution:'&copy; OpenStreetMap'}).addTo(map);
  mk=L.marker([s.lat,s.lon],{draggable:true}).addTo(map);
  mk.on('dragend',()=>pos(mk.getLatLng()));
  map.on('click',e=>{mk.setLatLng(e.latlng);pos(e.latlng)});
  ['lat','lon'].forEach(i=>$(i).onchange=()=>{mk.setLatLng([+$('lat').value,+$('lon').value]);map.panTo(mk.getLatLng())});
});
</script></body></html>)HTML";
