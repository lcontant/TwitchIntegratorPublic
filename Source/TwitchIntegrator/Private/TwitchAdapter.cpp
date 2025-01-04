// Fill out your copyright notice in the Description page of Project Settings.

#include "TwitchAdapter.h"

#include "Interfaces/IHttpResponse.h"


using namespace std;

mutex _connection_mutex;
TwitchAdapter::TwitchAdapter()
{
	_token = "";
	_username = "";
	_channel = "";
	token_check_interval = TOKEN_CHECK_INTERVAL_DEFAULT;
	commandPrefix = "";
	*twitchConnected = false;
	*twitchConnectionChanged = false;
	*subRecieved = false;
	*subGifted = false;
	*messageWaiting = false;
	*_cheerReceived = false;
	*commandEntered = false;
	*whisperReceived = false;
}

TwitchAdapter::TwitchAdapter(const std::string token, const std::string username, const std::string channel)
{
	_token = token;
	_username = username;
	_channel = channel;
	*twitchConnected = false;
	*twitchConnectionChanged = false;
	*subRecieved = false;
	*subGifted = false;
	*messageWaiting = false;
	*_cheerReceived = false;
	*commandEntered = false;
	*whisperReceived = false;
	token_check_interval = TOKEN_CHECK_INTERVAL_DEFAULT;
}

void TwitchAdapter::sendMessage(const FString message_to_send)
{
	string message_val = "PRIVMSG #" + _channel + " :";
	message_val += TCHAR_TO_UTF8(*message_to_send);
	message_val += "\r\n";
	_socketAdapter.sendServerMessageWithNoResponse(message_val);
}

void TwitchAdapter::Tick()
{
	if (last_token_check == NULL)
	{
		ConnectionAndTokenHealthCheck();
		last_token_check = FDateTime::Now();
	}
	else if (FDateTime::Now().GetSecond() - last_token_check.GetSecond() >= token_check_interval)
	{
		last_token_check = FDateTime::Now();
		ConnectionAndTokenHealthCheck();

		if (*_sessionIsActive && !_socketAdapter.checkInternetConnectionHealth())
		{
			toggleTwitchConnection(false);
			*_sessionIsActive = false;
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Connection is still valid to %s"), UTF8_TO_TCHAR(_channel.c_str()));
		}
	}
}


std::string TwitchAdapter::sendConnectionInfo()
{
	int num = 0;
	ULONG size = TwitchAdapter::TEST_CONNECTION_BUFFER_SIZE;
	std::string aouth = "PASS oauth:" + _token + "\r\n";
	std::string nick = "NICK " + _username + "\r\n";
	std::string joinChannel = "JOIN #" + _channel + "\r\n";
	std::string tags = "CAP REQ :twitch.tv/tags\r\n";
	std::string commands = "CAP REQ :twitch.tv/tags twitch.tv/commands\r\n";
	std::string userNotice = "USERNOTICE #" + _channel + "\r\n";

	_socketAdapter.sendServerMessageWithNoResponse(aouth);
	_socketAdapter.sendServerMessageWithNoResponse(nick);
	_socketAdapter.sendServerMessageWithNoResponse(tags);
	_socketAdapter.sendServerMessageWithNoResponse(commands);
	string response = _socketAdapter.sendServerMessageWithResponse(joinChannel);
	return response;
}

void TwitchAdapter::validateConnection()
{

	UE_LOG(LogTemp, Warning, TEXT("about to acquire mutex on channel %s"), UTF8_TO_TCHAR(_channel.c_str()));
	std::lock_guard<std::mutex> lck(_connection_mutex);
	UE_LOG(LogTemp, Warning, TEXT("acquired mutex on channel %s"), UTF8_TO_TCHAR(_channel.c_str()));
	if (!*twitchConnected && !_socketAdapter.checkInternetConnectionHealth())
	{
		*_sessionIsActive = false;
		toggleTwitchConnection(false);
	}
}

void TwitchAdapter::ConnectionAndTokenHealthCheck()
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetVerb("GET");
	HttpRequest->SetHeader("Content-Type", "application/json");

	HttpRequest->SetHeader("Authorization", ("Bearer " + this->_token).data());	

	HttpRequest->SetURL(*FString::Printf(TEXT("%hs"), TWITCH_TOKEN_VALIDATION_URL));


	HttpRequest->OnProcessRequestComplete().BindRaw(this, &TwitchAdapter::onValidateTokenResponse);

	HttpRequest->ProcessRequest();
}

void TwitchAdapter::SendRefreshTokenRequest()
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetVerb("Post");
	HttpRequest->SetHeader("Content-Type", "application/x-www-form-urlencoded");

	HttpRequest->SetURL(*FString::Printf(TEXT("%hs"), TWITCH_TOKEN_VALIDATION_URL));

	HttpRequest->OnProcessRequestComplete().BindRaw(this, &TwitchAdapter::onValidateTokenResponse);

	HttpRequest->ProcessRequest();
}

void TwitchAdapter::onValidateTokenResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	// FString JsonRaw = Response->GetContentAsString();
	// TSharedPtr<FJsonObject> JsonParsed;
	// TSharedRef<TJsonReader<TCHAR>> JsonReader = TJsonReaderFactory<TCHAR>::Create(JsonRaw);
	// if (FJsonSerializer::Deserialize(JsonReader, JsonParsed))
	// {
	// 	FString expiresIn = JsonParsed->GetStringField("expires_in");
	// }
}

std::string TwitchAdapter::connect()
{
	std::string serverConnectionErrorMsg = "";
	*_sessionIsActive = true;
	channelReference = UTF8_TO_TCHAR(("#" + _channel).c_str());
	UE_LOG(LogTemp, Warning, TEXT("About to establish server connection to channel %s"),
	       UTF8_TO_TCHAR(_channel.c_str()));
	serverConnectionErrorMsg = _socketAdapter.establishServerConnection(TWITCH_SERVER_ADDRESS, TWITCH_SERVER_PORT);
	UE_LOG(LogTemp, Warning, TEXT("Established server connection to channel %s"), UTF8_TO_TCHAR(_channel.c_str()));
	if (!serverConnectionErrorMsg.empty() || &_connection_mutex == NULL)
	{
		UE_LOG(LogTemp, Warning, TEXT("Connection failed channel %s"), UTF8_TO_TCHAR(_channel.c_str()));
		toggleTwitchConnection(false);
		return serverConnectionErrorMsg;
	}
	std::string response = sendConnectionInfo();
	UE_LOG(LogTemp, Warning, TEXT("Channel %s connection request Received response %s"),
	       UTF8_TO_TCHAR(_channel.c_str()), UTF8_TO_TCHAR(response.c_str()));
	if ((_messages.Num() > 0 && _messages[0].Contains("NOTICE")) || response.find("NOTICE") != string::npos || response.length() == 0 && (_messages.Num() > 0 && _messages[0].IsEmpty()))
	{
		toggleTwitchConnection(false);
		*_sessionIsActive = false;
		if (_messages.Num() > 0 && _messages[0].Contains("NOTICE"))
		{
			serverConnectionErrorMsg = TCHAR_TO_ANSI(*_messages[0]);
		}
		else if (response.find("NOTICE") != string::npos)
		{
			serverConnectionErrorMsg = response;
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("about to start validating connection for channel %s"),
		       UTF8_TO_TCHAR(_channel.c_str()));
		if (this != NULL)
		{
			_connectionValidationThread = std::thread(&TwitchAdapter::validateConnection, this);
			_connectionValidationThread.detach();
		}
		if (this != NULL)
		{
			UE_LOG(LogTemp, Warning, TEXT("about to start listening to chat on channel %s"),
			       UTF8_TO_TCHAR(_channel.c_str()));
			_chatListeningThread = std::thread(&TwitchAdapter::listenToChat, this);
			_chatMessageParsingThread = std::thread(&TwitchAdapter::ParseReceivedChatMessages, this);
			UE_LOG(LogTemp, Warning, TEXT("started listening to chat on channel %s"), UTF8_TO_TCHAR(_channel.c_str()));
			_chatListeningThread.detach();
			_chatMessageParsingThread.detach();
		}
	}

	return serverConnectionErrorMsg;
}
void TwitchAdapter::addCommand(FString command)
{
	_commands.push_back(command);
}
void TwitchAdapter::removeCommand(FString command)
{
	int commandIndex = -1;
	for (size_t i = 0; i < _commands.size(); i++)
	{
		if (command.Compare(_commands.at(i)) == 0)
		{
			commandIndex = i;
			break;
		}
	}

	if (commandIndex != -1)
	{
		_commands.erase(_commands.begin() + commandIndex);
	}
}

void TwitchAdapter::cleanUp()
{
	*_sessionIsActive = false;
	*twitchConnected = false;
	if (&_chatListeningThread != NULL && _chatListeningThread.joinable())
	{
		_chatListeningThread.join();
	}
	if (&_chatMessageParsingThread != NULL && _chatMessageParsingThread.joinable())
	{
		_chatMessageParsingThread.join();
	}
	_connection_mutex.unlock();
	_socketAdapter.cleanupSocket();
	_commands.clear();
}

void TwitchAdapter::Disconnect() {
	UE_LOG(LogTemp, Warning, TEXT("About to declare session inactive"))
	*_sessionIsActive = false;
	*twitchConnected = false;
	toggleTwitchConnection(*twitchConnected);
	UE_LOG(LogTemp, Warning, TEXT("Toggled connection"))
	if (&_chatListeningThread != NULL && _chatListeningThread.joinable())
	{
		_chatListeningThread.join();
	}
	UE_LOG(LogTemp, Warning, TEXT("Stopped listening to chat"))
	_connection_mutex.unlock();
	UE_LOG(LogTemp, Warning, TEXT("Unlocked connection mutex"))
	_socketAdapter.closeSocket();
	UE_LOG(LogTemp, Warning, TEXT("Closed socket"))
	_socketAdapter.cleanupSocket();
	UE_LOG(LogTemp, Warning, TEXT("Cleaned up socket"))
}


void TwitchAdapter::ParseReceivedChatMessages()
{
	FString currentMessage = "";
	std::string msg = "";
	std::string response = TWITCH_PING_RESPONSE_MESSAGE;
	int num = 1;
	while ( *_sessionIsActive )
	{
		std::lock_guard<std::mutex> MessagesLock(*_dataLock.GetArrayMutex());
		while (messages.Num() > 0)
		{
			if (*_sessionIsActive == false)
			{
				break;	
			}
			_connection_mutex.unlock();
			msg = messages[0];
			messages.RemoveAt(0);
			if (msg == TWITCH_PING_MESSAGE)
			{
			
				UE_LOG(LogTemp, Log, TEXT("Reliability: ABOUT TO SEND PONG"));
				_socketAdapter.sendServerMessageWithNoResponse(response);
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("about to parse: %s"), UTF8_TO_TCHAR(msg.c_str()));
				FChatMessageData data = parseMessage(msg);
				currentMessage = msg.c_str(); 
				if (!*twitchConnected && currentMessage.Contains(channelReference) && _socketAdapter.checkInternetConnectionHealth())
				{
					toggleTwitchConnection(true);
				}
				else if (currentMessage.StartsWith("@badge") && !currentMessage.Contains("JOIN", ESearchCase::CaseSensitive, ESearchDir::FromStart) && !currentMessage.Contains("USERSTATE", ESearchCase::CaseSensitive, ESearchDir::FromStart))
				{
					currentMessage = msg.c_str();
					_data.Add(data);
					UE_LOG(LogTemp, Log, TEXT("Manipulation: Adding to data with now : %d items"), _data.Num());
					_messages.Add(currentMessage);
					UE_LOG(LogTemp, Log, TEXT("Manipulation: Adding to messages with now : %d items"), _messages.Num());
					*messageWaiting = true;
				}
				else if (!_socketAdapter.checkTwitchConnectionHealth())
				{
					_error_message = currentMessage;
					toggleTwitchConnection(false);
					*_sessionIsActive = false;
					*shouldReconnect = false;
					break;
				}
			}
		}
	}
}

void TwitchAdapter::listenToChat()
{
	std::string msg = "";
	std::string bufferMsg = "";
	std::string currentMessage = "";
	std::string currentPayload = "";
	std::string stringToBeAdded = "";
	int num = 0;
	

		while (*_sessionIsActive)
		{
			//while the current payload does not contains a return carriages , we have processed all the messages and the
			// The session is active
			while (currentPayload.find("\r\n") == std::string::npos && messages.Num() == 0 && *_sessionIsActive)
			{
				bufferMsg = "";
				num = _socketAdapter.receiveMessage(bufferMsg);
				if (num > 0)
				{
					UE_LOG(LogTemp, Log, TEXT("payload received: %s"), UTF8_TO_TCHAR(bufferMsg.c_str()));
					//Concact the bufferMsg at the end of the current payload
					currentPayload += bufferMsg;
				}
				while (num > 0 && currentPayload.find("\r\n") != std::string::npos)
				{
					std::lock_guard<std::mutex> MessagesLock(*_dataLock.GetArrayMutex());
					//Split the current payload on the return line and put the first in the messages to be processed
					msg = currentPayload.substr(0, currentPayload.find("\r\n"));
					//convert to msg to an FString
					messages.Add(msg);
					if (stringToBeAdded.length() > 0)
					{
						UE_LOG(LogTemp, Log, TEXT("Message split currentPayload: %hs ##### stringToBeAdded: %hs"), currentPayload.c_str(), stringToBeAdded.c_str());
					}
					currentPayload = stringToBeAdded;
				}
				bufferMsg = "";
			}
		}
		if (*twitchConnected)
		{
			toggleTwitchConnection(false);
		}
		_socketAdapter.closeSocket();
}
FChatMessageData TwitchAdapter::parseMessage(const std::string &msg)
{
	/*String stream to convert values for the _data fields*/
	int value = -1;
	FString fullMatchedSection = "";
	FString value_temp = "";
	bool colorFound = false;
	bool bitsFound = false;
	int red = 0;
	int green = 0;
	int blue = 0;
	FString msg_temp = FString(UTF8_TO_TCHAR(msg.c_str()));
	FChatMessageField field_temp;
	FChatMessageData currentData;
	currentData.bits_sent = false;
	currentData.raw_message = FString(UTF8_TO_TCHAR(msg.c_str()));
	currentData.containsCommands = false;
	//Initialize the commands entered to an empty array
	currentData.commands_entered = TArray<FString>();
	FString regex_str = UTF8_TO_TCHAR(_regex_str);	
	const FRegexPattern myPattern(regex_str);
	FRegexMatcher messageMatcherField(myPattern, msg_temp);
	while (messageMatcherField.FindNext())
	{
		fullMatchedSection = messageMatcherField.GetCaptureGroup(0);
		if (fullMatchedSection.Contains(";bits") && !bitsFound)
		{
			*_cheerReceived = true;
			currentData.bits_sent = true;
			value_temp = messageMatcherField.GetCaptureGroup(2);
			currentData.number_of_bits = FCString::Atoi(*value_temp);
		}
		else if (fullMatchedSection.Contains("subscriber"))
		{
			FString test = messageMatcherField.GetCaptureGroup(1);
			value_temp = messageMatcherField.GetCaptureGroup(2);
			value = FCString::Atoi(*value_temp);
			currentData.sender_is_subbed = value > 0;
			UE_LOG(LogTemp, Log, TEXT("Sender is subbed value %d on %s"), currentData.sender_is_subbed, UTF8_TO_TCHAR(msg.c_str()));
		}
		else if (fullMatchedSection.Contains(";display-name"))
		{
			UE_LOG(LogTemp, Log, TEXT("Display name found on %s"), *FString(fullMatchedSection));
			value_temp = messageMatcherField.GetCaptureGroup(2);
			currentData.sender_username = value_temp;
			currentData.reciever_username = value_temp;
		}
		else if (fullMatchedSection.Contains("color") && messageMatcherField.GetCaptureGroup(2).Len() >= 7)
		{
			colorFound = true;
			value_temp = messageMatcherField.GetCaptureGroup(2);
			vector<FString> str_values(3);
			str_values[0] = value_temp.Mid(1, 2);
			str_values[1] = value_temp.Mid(3, 2);
			str_values[2] = value_temp.Mid(5, 2);

			red = FParse::HexNumber(*str_values[0]);
			green = FParse::HexNumber(*str_values[1]);
			blue = FParse::HexNumber(*str_values[2]);

			FColor color(red, green, blue, TwitchAdapter::ALPHA_CHAT_COLOR);
			currentData.sender_username_color_byte = color;
			currentData.sender_username_color = FLinearColor(color);
		}
		else if (fullMatchedSection.Contains("mod="))
		{
			value_temp = messageMatcherField.GetCaptureGroup(2);
			value = FCString::Atoi(*value_temp);
			currentData.isModerator = value > 0;
		}
		else if (fullMatchedSection.Contains("badge"))
		{
			currentData.isVIP = fullMatchedSection.Contains("vip");
		}
		else if (fullMatchedSection.Contains("msg-id"))
		{
			if (fullMatchedSection.Contains("sub"))
			{
				*subRecieved = true;
				if (fullMatchedSection.Contains("gift"))
				{
					*subGifted = true;
					currentData.is_gift_sub = true;
				}
			}
			else if (fullMatchedSection.Contains("highlighted-message"))
			{
				currentData.is_highlight_message = true;
			}
			value_temp = messageMatcherField.GetCaptureGroup(2);
			currentData.notice_message = value_temp;
		}
		else if (fullMatchedSection.Contains("msg-param-cumulative-months") || fullMatchedSection.Contains("msg-param-months") || fullMatchedSection.Contains("msg-param-streak-months"))
		{
			value_temp = messageMatcherField.GetCaptureGroup(2);
			value = FCString::Atoi(*value_temp);
			currentData.sub_month_total = value;
		}
		else if (fullMatchedSection.Contains("msg-param-recipient-display-name"))
		{
			value_temp = messageMatcherField.GetCaptureGroup(2);
			currentData.reciever_username = value_temp;
		}
		else if (fullMatchedSection.Contains("ms-param-sender-name"))
		{
			value_temp = messageMatcherField.GetCaptureGroup(2);
			currentData.gifter_username = value_temp;
		}
		else
		{
			value_temp = messageMatcherField.GetCaptureGroup(1);
			field_temp.field_name = value_temp;
			value_temp = messageMatcherField.GetCaptureGroup(2);
			field_temp.field_value = value_temp;
			currentData.other_Data.Add(field_temp);
		}
	}
	if (currentData.sender_username.IsEmpty())
	{
		messageMatcherField = FRegexMatcher(_ue_username_fallback_regex, msg_temp);
		
		if (messageMatcherField.FindNext())
		{
			value_temp = messageMatcherField.GetCaptureGroup(1);
			currentData.sender_username = value_temp;
		}
	}
	//Cast to TCHAR to use FRegexMatcher
	messageMatcherField = FRegexMatcher(_ue_message_regex, msg_temp); 
	if (messageMatcherField.FindNext() && messageMatcherField.GetCaptureGroup(1).Len() > 0)
	{
		currentData.message = messageMatcherField.GetCaptureGroup(1);
		currentData.messsageText = FText::FromString(currentData.message);
		currentData.isWhisper = false;
		*whisperReceived = false;
	}
	else if (this != NULL)
	{
		messageMatcherField = FRegexMatcher(_ue_whisper_message_regex, msg_temp);
		if (messageMatcherField.FindNext())
		{
			currentData.message = messageMatcherField.GetCaptureGroup(1);
			currentData.messsageText = FText::FromString(currentData.message);
			currentData.isWhisper = true;
			*whisperReceived = true;
		}
	}
	for (size_t i = 0; i < _commands.size(); i++)
	{
		if (currentData.message.Contains(UTF8_TO_TCHAR(commandPrefix.c_str()) + _commands.at(i)))
		{
			currentData.commands_entered.Add(_commands.at(i));
			currentData.containsCommands = true;
			*commandEntered = true;
		}
	}
	if (!colorFound)
	{
		srand(time(NULL));
		red = rand() % TwitchAdapter::BYTE_COLOR_MAX_VALUE;
		green = rand() % TwitchAdapter::BYTE_COLOR_MAX_VALUE;
		blue = rand() % TwitchAdapter::BYTE_COLOR_MAX_VALUE;
		FColor color(red, green, blue, TwitchAdapter::ALPHA_CHAT_COLOR);
		currentData.sender_username_color_byte = color;
		currentData.sender_username_color = FLinearColor(color);
		currentData.colorIsAccurate = false;
	}
	else
	{
		currentData.colorIsAccurate = true;
	}
	return currentData;
}

void TwitchAdapter::sendWhisper(const FString message_to_send, const FString receiver)
{
	this->sendMessage("/w " + receiver + " " + message_to_send);
}


void TwitchAdapter::toggleTwitchConnection(const bool state)
{
	*twitchConnectionChanged = true;
	*twitchConnected = state;
}
