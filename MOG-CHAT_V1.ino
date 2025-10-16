#include <WiFi.h>
#include <WebServer.h>

const char* ssid = "CHAT";
const char* password = "mogchat123";

WebServer server(80);

String chatMessages = "";
int messageCount = 0;

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>MOG-CHAT</title>
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
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>MOG-CHAT</h1>
            <div style="color: #72767d; font-size: 14px;">by MOG-Developing</div>
        </div>
        <div class="chat-area" id="chatArea"></div>
        <div class="input-area">
            <div class="input-container">
                <input type="text" id="messageInput" placeholder="Send a message in MOG-CHAT" maxlength="2000">
                <button id="sendButton">Send</button>
            </div>
        </div>
    </div>

    <script>
        const chatArea = document.getElementById('chatArea');
        const messageInput = document.getElementById('messageInput');
        const sendButton = document.getElementById('sendButton');
        let username = 'User' + Math.floor(Math.random() * 1000);

        function formatTime(date) {
            return date.toLocaleTimeString('en-US', { 
                hour12: false, 
                hour: '2-digit', 
                minute: '2-digit' 
            });
        }

        function addMessage(user, msg, time) {
            const messageDiv = document.createElement('div');
            messageDiv.className = 'message';
            messageDiv.innerHTML = `
                <div class="message-header">
                    <span class="username">${user}</span>
                    <span class="timestamp">${time}</span>
                </div>
                <div class="message-content">${msg}</div>
            `;
            chatArea.appendChild(messageDiv);
            chatArea.scrollTop = chatArea.scrollHeight;
        }

        function sendMessage() {
            const message = messageInput.value.trim();
            if (message) {
                fetch('/send', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                    },
                    body: 'user=' + encodeURIComponent(username) + '&message=' + encodeURIComponent(message)
                }).then(response => {
                    if (response.ok) {
                        messageInput.value = '';
                        loadMessages();
                    }
                });
            }
        }

        function loadMessages() {
            fetch('/messages')
                .then(response => response.text())
                .then(messages => {
                    chatArea.innerHTML = messages;
                    chatArea.scrollTop = chatArea.scrollHeight;
                });
        }

        sendButton.addEventListener('click', sendMessage);
        
        messageInput.addEventListener('keypress', function(e) {
            if (e.key === 'Enter') {
                sendMessage();
            }
        });

        setInterval(loadMessages, 1000);
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
  if (server.hasArg("user") && server.hasArg("message")) {
    String user = server.arg("user");
    String message = server.arg("message");
    
    if (message.length() > 0 && message.length() <= 2000) {
      String timestamp = String(millis());
      String messageHTML = "<div class='message'><div class='message-header'><span class='username'>" + 
                          user + "</span><span class='timestamp'>" + timestamp + "</span></div>" +
                          "<div class='message-content'>" + message + "</div></div>";
      
      chatMessages += messageHTML;
      messageCount++;
      
      if (messageCount > 100) {
        int firstMessageEnd = chatMessages.indexOf("</div>") + 6;
        chatMessages = chatMessages.substring(firstMessageEnd);
        messageCount--;
      }
    }
  }
  server.send(200, "text/plain", "OK");
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