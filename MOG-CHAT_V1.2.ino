#include <WiFi.h>
#include <WebServer.h>
#include <Arduino.h>

const char* ssid = "CHAT";
const char* password = "mogchat123";

WebServer server(80);

const byte MAX_MESSAGES_CONST = 32;
const size_t MAX_MSG_LEN_CONST = 512;

struct Message {
  char user[21];
  char msg[MAX_MSG_LEN_CONST + 1];
  unsigned long time;
};

Message messages[MAX_MESSAGES_CONST];
byte msgIndex = 0;
byte msgTotal = 0;

bool validateUser(const String& user) {
  if (user.length() < 1 || user.length() > 20) return false;
  for (size_t i = 0; i < user.length(); i++) {
    char c = user.charAt(i);
    if (!isalnum(c) && c != '_' && c != '-') return false;
  }
  return true;
}

bool validateMessage(const String& message) {
  return message.length() > 0 && message.length() <= MAX_MSG_LEN_CONST;
}

void handleRoot() {
  String html = F("<!DOCTYPE html><html><head><title>MOG-CHAT V1.2</title><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><style>body{font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,sans-serif;background:#36393f;color:#dcddde;margin:0;padding:0}.container{max-width:800px;margin:0 auto;height:100vh;display:flex;flex-direction:column}.header{background:#2f3136;padding:12px;border-bottom:1px solid #202225;text-align:center}.chat-area{flex:1;overflow-y:auto;padding:12px;background:#36393f}.message{margin-bottom:12px;padding:6px 12px}.message:hover{background:#32353b;border-radius:4px}.message-header{display:flex;align-items:center;margin-bottom:2px}.username{font-weight:500;color:#fff;margin-right:6px}.timestamp{color:#72767d;font-size:11px}.message-content{color:#dcddde;word-wrap:break-word}.input-area{background:#40444b;padding:12px;border-top:1px solid #202225}.input-container{display:flex;background:#484c52;border-radius:6px;padding:6px}#messageInput{flex:1;background:transparent;border:none;color:#dcddde;font-size:14px;padding:6px;outline:none}#sendButton{background:#5865f2;color:#fff;border:none;border-radius:3px;padding:6px 12px;margin-left:6px;cursor:pointer;font-weight:500}#sendButton:hover{background:#4752c4}#sendButton:disabled{opacity:0.6;cursor:not-allowed}</style></head><body><div class=\"container\"><div class=\"header\"><h1 style=\"margin:8px 0\">MOG-CHAT V1.2</h1><div style=\"color:#72767d;font-size:12px\">by MOG-Developing</div></div><div class=\"chat-area\" id=\"chatArea\"></div><div class=\"input-area\"><div class=\"input-container\"><input type=\"text\" id=\"messageInput\" placeholder=\"Send a message\" maxlength=\"2000\"><button id=\"sendButton\">Send</button></div></div></div><script>const chatArea=document.getElementById('chatArea'),messageInput=document.getElementById('messageInput'),sendButton=document.getElementById('sendButton');let username='User'+Math.floor(Math.random()*10000),isSending=!1;function addMessage(u,m,t){const d=document.createElement('div');d.className='message';d.innerHTML='<div class=\"message-header\"><span class=\"username\">'+u+'</span><span class=\"timestamp\">'+new Date().toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'})+'</span></div><div class=\"message-content\">'+m+'</div>';chatArea.appendChild(d);chatArea.scrollTop=chatArea.scrollHeight}async function sendMessage(){if(isSending)return;const m=messageInput.value.trim();if(m){isSending=!0;sendButton.disabled=!0;sendButton.textContent='Sending...';try{const fd=new FormData;fd.append('user',username);fd.append('message',m);const r=await fetch('/send',{method:'POST',body:fd});if(r.ok){messageInput.value='';await loadMessages()}}catch(e){console.error('Error:',e)}finally{isSending=!1;sendButton.disabled=!1;sendButton.textContent='Send'}}}async function loadMessages(){try{const r=await fetch('/messages');if(r.ok){const m=await r.text();if(m&&m.trim()){chatArea.innerHTML=m;chatArea.scrollTop=chatArea.scrollHeight}}}catch(e){console.error('Load error:',e)}}sendButton.addEventListener('click',sendMessage);messageInput.addEventListener('keypress',e=>{if('Enter'===e.key)sendMessage()});setInterval(loadMessages,1000);loadMessages();</script></body></html>");
  server.send(200, F("text/html"), html);
}

void handleMessages() {
  String output = "";
  byte start = msgTotal < MAX_MESSAGES_CONST ? 0 : msgIndex;
  byte count = msgTotal < MAX_MESSAGES_CONST ? msgTotal : MAX_MESSAGES_CONST;
  
  for (byte i = 0; i < count; i++) {
    byte idx = (start + i) % MAX_MESSAGES_CONST;
    output += F("<div class=\"message\"><div class=\"message-header\"><span class=\"username\">");
    output += messages[idx].user;
    output += F("</span><span class=\"timestamp\">");
    output += String(messages[idx].time);
    output += F("</span></div><div class=\"message-content\">");
    output += messages[idx].msg;
    output += F("</div></div>");
  }
  server.send(200, F("text/html"), output);
}

void handleSend() {
  if (server.hasArg("user") && server.hasArg("message")) {
    String user = server.arg("user");
    String message = server.arg("message");
    
    if (validateUser(user) && validateMessage(message)) {
      user.toCharArray(messages[msgIndex].user, 21);
      
      String safeMsg = message;
      safeMsg.replace("&", "&amp;");
      safeMsg.replace("<", "&lt;");
      safeMsg.replace(">", "&gt;");
      safeMsg.replace("\"", "&quot;");
      safeMsg.toCharArray(messages[msgIndex].msg, MAX_MSG_LEN_CONST + 1);
      
      messages[msgIndex].time = millis();
      
      msgIndex = (msgIndex + 1) % MAX_MESSAGES_CONST;
      if (msgTotal < MAX_MESSAGES_CONST) msgTotal++;
      
      server.send(200, F("text/plain"), F("OK"));
      return;
    }
  }
  server.send(400, F("text/plain"), F("ERROR"));
}

String base64_decode(String input) {
  String result = "";
  int in_len = input.length();
  int i = 0;
  
  while (i < in_len) {
    unsigned char in[4] = {0};
    for (int j = 0; j < 4 && i < in_len; j++, i++) {
      char c = input.charAt(i);
      if (c >= 'A' && c <= 'Z') in[j] = c - 'A';
      else if (c >= 'a' && c <= 'z') in[j] = c - 'a' + 26;
      else if (c >= '0' && c <= '9') in[j] = c - '0' + 52;
      else if (c == '+') in[j] = 62;
      else if (c == '/') in[j] = 63;
      else if (c == '=') in[j] = 64;
      else continue;
    }
    
    if (in[0] != 64 && in[1] != 64) {
      result += char((in[0] << 2) | ((in[1] & 0x30) >> 4));
      if (in[2] != 64) {
        result += char(((in[1] & 0x0F) << 4) | ((in[2] & 0x3C) >> 2));
        if (in[3] != 64) {
          result += char(((in[2] & 0x03) << 6) | in[3]);
        }
      }
    }
  }
  return result;
}

bool checkAuth() {
  if (!server.hasHeader("Authorization")) {
    server.sendHeader("WWW-Authenticate", "Basic realm=\"MOG-CHAT Info\"");
    server.send(401, F("text/plain"), F("Unauthorized"));
    return false;
  }
  
  String auth = server.header("Authorization");
  if (auth.startsWith("Basic ")) {
    auth = auth.substring(6);
    auth = base64_decode(auth);
    
    if (auth == "admin:admin") {
      return true;
    }
  }
  
  server.sendHeader("WWW-Authenticate", "Basic realm=\"MOG-CHAT Info\"");
  server.send(401, F("text/plain"), F("Unauthorized"));
  return false;
}

void handleInfo() {
  if (!checkAuth()) return;
  
  String info = F("<!DOCTYPE html><html><head><title>MOG-CHAT V1.2 - Info</title><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><style>body{font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,sans-serif;background:#36393f;color:#dcddde;margin:20px}.container{max-width:600px;margin:0 auto;background:#2f3136;padding:20px;border-radius:8px}h1{color:#fff;margin-top:0}.info-item{margin:12px 0;padding:8px;background:#36393f;border-radius:4px}.label{color:#72767d;font-size:12px}.value{color:#fff;font-size:16px;margin-top:4px}</style></head><body><div class=\"container\"><h1>MOG-CHAT V1.2 - System Info</h1><div class=\"info-item\"><div class=\"label\">Device</div><div class=\"value\">ESP32WROOM32U</div></div><div class=\"info-item\"><div class=\"label\">Firmware</div><div class=\"value\">MOG-CHAT V1.2</div></div><div class=\"info-item\"><div class=\"label\">Developer</div><div class=\"value\">MOG-Developing</div></div><div class=\"info-item\"><div class=\"label\">SSID</div><div class=\"value\">");
  info += ssid;
  info += F("</div></div><div class=\"info-item\"><div class=\"label\">IP Address</div><div class=\"value\">");
  info += WiFi.softAPIP().toString();
  info += F("</div></div><div class=\"info-item\"><div class=\"label\">Connected Clients</div><div class=\"value\">");
  info += String(WiFi.softAPgetStationNum());
  info += F("</div></div><div class=\"info-item\"><div class=\"label\">Messages Stored</div><div class=\"value\">");
  info += String(msgTotal);
  info += F("</div></div><div class=\"info-item\"><div class=\"label\">Uptime</div><div class=\"value\">");
  info += String(millis() / 1000);
  info += F(" seconds</div></div><div class=\"info-item\"><div class=\"label\">Free Heap</div><div class=\"value\">");
  info += String(ESP.getFreeHeap());
  info += F(" bytes</div></div></div></body></html>");
  
  server.send(200, F("text/html"), info);
}

void handleNotFound() {
  server.send(404, F("text/plain"), F("404"));
}


void setup() {
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.softAP(ssid, password, 1, 0, 4);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  
  server.on(F("/"), handleRoot);
  server.on(F("/messages"), handleMessages);
  server.on(F("/send"), HTTP_POST, handleSend);
  server.on(F("/info"), handleInfo);
  server.onNotFound(handleNotFound);
  
  server.begin();
}

void loop() {
  server.handleClient();
}
