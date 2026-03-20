-- HelloWorld addon — demonstrates the WoWee addon system

-- Initialize saved variables (persisted across sessions)
if not HelloWorldDB then
    HelloWorldDB = { loginCount = 0 }
end
HelloWorldDB.loginCount = (HelloWorldDB.loginCount or 0) + 1

-- Create a frame and register for events (standard WoW addon pattern)
local f = CreateFrame("Frame", "HelloWorldFrame")
f:RegisterEvent("PLAYER_ENTERING_WORLD")
f:RegisterEvent("CHAT_MSG_SAY")

f:SetScript("OnEvent", function(self, event, ...)
    if event == "PLAYER_ENTERING_WORLD" then
        local name = UnitName("player")
        local level = UnitLevel("player")
        print("|cff00ff00[HelloWorld]|r Welcome, " .. name .. "! (Level " .. level .. ")")
        print("|cff00ff00[HelloWorld]|r Login count: " .. HelloWorldDB.loginCount)
    elseif event == "CHAT_MSG_SAY" then
        local msg, sender = ...
        if msg and sender then
            print("|cff00ff00[HelloWorld]|r " .. sender .. " said: " .. msg)
        end
    end
end)

-- Register a custom slash command
SLASH_HELLOWORLD1 = "/hello"
SLASH_HELLOWORLD2 = "/hw"
SlashCmdList["HELLOWORLD"] = function(args)
    print("|cff00ff00[HelloWorld]|r Hello! " .. (args ~= "" and args or "Type /hello <message>"))
    print("|cff00ff00[HelloWorld]|r Sessions: " .. HelloWorldDB.loginCount)
end

print("|cff00ff00[HelloWorld]|r Addon loaded. Type /hello to test.")
