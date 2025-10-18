#include <WiFi.h>
#include <WebServer.h>

const char* ssid = "CHAT";
const char* password = "mogchat123";

WebServer server(80);

String chatMessages = "";
int messageCount = 0;

String sanitizeHTML(String input) {
  input.replace("&", "&amp;");
  input.replace("<", "&lt;");
  input.replace(">", "&gt;");
  input.replace("\"", "&quot;");
  input.replace("'", "&#x27;");
  input.replace("/", "&#x2F;");
  return input;
}

bool validateUser(String user) {
  if (user.length() < 1 || user.length() > 20) return false;
  for (size_t i = 0; i < user.length(); i++) {
    char c = user.charAt(i);
    if (!isalnum(c) && c != '_' && c != '-') return false;
  }
  return true;
}

bool validateMessage(String message) {
  return message.length() > 0 && message.length() <= 2000;
}

unsigned long simpleHash(String input) {
  unsigned long hash = 5381;
  for (size_t i = 0; i < input.length(); i++) {
    hash = ((hash << 5) + hash) + input.charAt(i);
  }
  return hash;
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>MOG-CHAT V1.1</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body {
            font-family: 'Whitney', 'Helvetica Neue', Helvetica, Arial, sans-serif;
            background-color: #36393f;
            color: #dcddde;
            margin: 0;
            padding: 0;
        }
        .container {
            max-width: 800px;
            margin: 0 auto;
            height: 100vh;
            display: flex;
            flex-direction: column;
        }
        .header {
            background-color: #2f3136;
            padding: 16px;
            border-bottom: 1px solid #202225;
            text-align: center;
        }
        .chat-area {
            flex: 1;
            overflow-y: auto;
            padding: 16px;
            background-color: #36393f;
        }
        .message {
            margin-bottom: 16px;
            padding: 8px 16px;
        }
        .message-header {
            display: flex;
            align-items: center;
            margin-bottom: 4px;
        }
        .username {
            font-weight: 500;
            color: white;
            margin-right: 8px;
        }
        .timestamp {
            color: #72767d;
            font-size: 12px;
        }
        .message-content {
            color: #dcddde;
            word-wrap: break-word;
        }
        .input-area {
            background-color: #40444b;
            padding: 16px;
            border-top: 1px solid #202225;
        }
        .input-container {
            display: flex;
            background-color: #484c52;
            border-radius: 8px;
            padding: 8px;
        }
        #messageInput {
            flex: 1;
            background: transparent;
            border: none;
            color: #dcddde;
            font-size: 16px;
            padding: 8px;
            outline: none;
        }
        #sendButton {
            background-color: #5865f2;
            color: white;
            border: none;
            border-radius: 4px;
            padding: 8px 16px;
            margin-left: 8px;
            cursor: pointer;
            font-weight: 500;
        }
        #sendButton:hover {
            background-color: #4752c4;
        }
        .message:hover {
            background-color: #32353b;
            border-radius: 4px;
        }
        .security-notice {
            text-align: center;
            font-size: 12px;
            color: #72767d;
            margin-top: 5px;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>MOG-CHAT V1.1</h1>
            <div style="color: #72767d; font-size: 14px;">by MOG-Developing</div>
            <div class="security-notice">Thanks for using MOG-CHAT!</div>
        </div>
        <div class="chat-area" id="chatArea"></div>
        <div class="input-area">
            <div class="input-container">
                <input type="text" id="messageInput" placeholder="Send a message in MOG-CHAT V1.1" maxlength="2000">
                <button id="sendButton">Send</button>
            </div>
        </div>
    </div>

    <script>
        const chatArea = document.getElementById('chatArea');
        const messageInput = document.getElementById('messageInput');
        const sendButton = document.getElementById('sendButton');
        let username = 'User' + Math.floor(Math.random() * 10000);
        let messageCounter = 0;
        let isSending = false;

        function addMessage(user, msg, time) {
            const messageDiv = document.createElement('div');
            messageDiv.className = 'message';
            messageDiv.setAttribute('data-msgid', messageCounter++);
            messageDiv.innerHTML = `
                <div class="message-header">
                    <span class="username">${user}</span>
                    <span class="timestamp">${new Date().toLocaleTimeString([], {hour: '2-digit', minute:'2-digit'})}</span>
                </div>
                <div class="message-content">${msg}</div>
            `;
            chatArea.appendChild(messageDiv);
            chatArea.scrollTop = chatArea.scrollHeight;
        }

        function simpleHash(str) {
            let hash = 5381;
            for (let i = 0; i < str.length; i++) {
                hash = ((hash << 5) + hash) + str.charCodeAt(i);
            }
            return hash;
        }

        async function sendMessage() {
            if (isSending) return;
            
            const message = messageInput.value.trim();
            if (message) {
                isSending = true;
                sendButton.disabled = true;
                sendButton.textContent = 'Sending...';
                
                try {
                    const timestamp = Date.now();
                    const hash = simpleHash(timestamp + '|' + username + '|' + message);
                    
                    const formData = new FormData();
                    formData.append('user', username);
                    formData.append('message', message);
                    formData.append('timestamp', timestamp);
                    formData.append('hash', hash);
                    
                    const response = await fetch('/send', {
                        method: 'POST',
                        body: formData
                    });
                    
                    if (response.ok) {
                        messageInput.value = '';
                        await loadMessages();
                    } else {
                        console.error('Send failed:', response.status);
                    }
                } catch (error) {
                    console.error('Send error:', error);
                } finally {
                    isSending = false;
                    sendButton.disabled = false;
                    sendButton.textContent = 'Send';
                }
            }
        }

        async function loadMessages() {
            try {
                const response = await fetch('/messages');
                if (!response.ok) throw new Error('Network error');
                const messages = await response.text();
                if (messages && messages.trim() !== '') {
                    chatArea.innerHTML = messages;
                    chatArea.scrollTop = chatArea.scrollHeight;
                }
            } catch (error) {
                console.error('Load failed:', error);
            }
        }

        sendButton.addEventListener('click', sendMessage);
        
        messageInput.addEventListener('keypress', function(e) {
            if (e.key === 'Enter') {
                sendMessage();
            }
        });

        setInterval(loadMessages, 1500);
        loadMessages();
    </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleMessages() {
  server.send(200, "text/html", chatMessages);
}

void handleSend() {
  if (server.hasArg("user") && server.hasArg("message") && server.hasArg("timestamp") && server.hasArg("hash")) {
    String user = server.arg("user");
    String message = server.arg("message");
    String timestamp = server.arg("timestamp");
    String receivedHash = server.arg("hash");
    
    if (validateUser(user) && validateMessage(message)) {
      String reconstructed = timestamp + "|" + user + "|" + message;
      unsigned long expectedHash = simpleHash(reconstructed);
      unsigned long actualHash = strtoul(receivedHash.c_str(), NULL, 10);
      
      if (expectedHash == actualHash) {
        String safeUser = sanitizeHTML(user);
        String safeMessage = sanitizeHTML(message);
        
        String messageHTML = "<div class='message'><div class='message-header'><span class='username'>" + 
                            safeUser + "</span><span class='timestamp'>" + String(millis()) + "</span></div>" +
                            "<div class='message-content'>" + safeMessage + "</div></div>";
        
        chatMessages += messageHTML;
        messageCount++;
        
        if (messageCount > 100) {
          int firstMessageEnd = chatMessages.indexOf("</div>") + 6;
          chatMessages = chatMessages.substring(firstMessageEnd);
          messageCount--;
        }
        server.send(200, "text/plain", "OK");
        return;
      }
    }
  }
  server.send(400, "text/plain", "ERROR");
}

void handleNotFound() {
  server.send(404, "text/plain", "404: Not found");
}

void setup() {
  WiFi.softAP(ssid, password);
  
  IPAddress IP = WiFi.softAPIP();
  
  server.on("/", handleRoot);
  server.on("/messages", handleMessages);
  server.on("/send", HTTP_POST, handleSend);
  server.onNotFound(handleNotFound);
  
  server.begin();
}

void loop() {
  server.handleClient();
}
