#pragma once
// Browser side of the SD card access (served on "/").
// Written with the Q() stringify macro: no ' or # characters, no // comments, no lines starting with #.

#define Q(...) #__VA_ARGS__

static const char http_page[] = Q(
<!doctype html><meta name=viewport content="width=device-width"><title>PicoMEM SD</title>
<style>body{font:14px sans-serif;margin:1em}td{padding:2px 10px}a{cursor:pointer;color:blue}</style>
<h3>PicoMEM SD <span id=p></span></h3>
<p>Wifi MAC: <span id=w></span></p>
<input type=file id=f multiple> <button onclick=U()>Upload</button> <button onclick=N()>New folder</button> <span id=m></span>
<table id=t></table>
<script>
var P="/",K=sessionStorage.k||"",$=function(i){return document.getElementById(i)};
function X(e){$("m").textContent=e.message||e}
function Z(a){return "?p="+encodeURIComponent(a)}
function Y(a){return a=="/"?"":a}
function Q(u,o){o=o||{};return fetch(u+"&k="+encodeURIComponent(K),o).then(function(r){if(r.status==401&&!o.r){K=prompt("Password")||"";sessionStorage.k=K;o.r=1;return Q(u,o)}if(!r.ok)throw new Error("HTTP "+r.status);return r})}
function R(a,n,s,q){var t=document.createElement("tr"),d=document.createElement("td"),l=document.createElement("a");
l.textContent=a=="d"?"["+n+"]":n;
if(a=="d")l.onclick=function(){L(q)};else{l.href="/f"+Z(q)+"&k="+encodeURIComponent(K);l.download=n}
d.appendChild(l);t.appendChild(d);
d=document.createElement("td");d.textContent=a=="d"?"":s;t.appendChild(d);
if(n!=".."){d=document.createElement("td");l=document.createElement("a");l.textContent="delete";
l.onclick=function(){if(confirm("Delete "+n+"?"))Q("/f"+Z(q),{method:"DELETE"}).then(function(){L(P)}).catch(X)};
d.appendChild(l);t.appendChild(d)}
$("t").appendChild(t)}
function L(p){P=p;$("p").textContent=p;$("t").innerHTML="";
Q("/ls"+Z(p)).then(function(r){return r.text()}).then(function(x){var q=p.lastIndexOf("/");
if(p!="/")R("d","..",0,q?p.slice(0,q):"/");
x.split("\n").forEach(function(l){var a=l.split("\t");if(a.length>2){var n=a.slice(2).join("\t");R(a[0],n,a[1],Y(p)+"/"+n)}})}).catch(X)}
function U(){var fs=$("f").files,i=0;(function nx(){if(i>=fs.length){$("m").textContent="done";L(P);return}
var f=fs[i++];$("m").textContent="Uploading "+f.name+" ...";
Q("/f"+Z(Y(P)+"/"+f.name),{method:"PUT",body:f}).then(nx).catch(X)})()}
function N(){var n=prompt("Folder name");if(n)Q("/d"+Z(Y(P)+"/"+n),{method:"POST"}).then(function(){L(P)}).catch(X)}
fetch("/mac").then(function(r){return r.text()}).then(function(x){$("w").textContent=x});
L("/")
</script>
);
