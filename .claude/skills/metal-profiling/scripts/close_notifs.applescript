tell application "System Events"
	set n to 0
	try
		tell process "NotificationCenter"
			if (count of windows) is 0 then return 0
			set elems to entire contents of window 1
			repeat with e in elems
				try
					repeat with a in (actions of e)
						set nm to (name of a) as text
						set ds to ""
						try
							set ds to (description of a) as text
						end try
						if nm contains "Close" or nm contains "Clear" or ds is "Close" or ds is "Clear" then
							perform a
							set n to n + 1
							exit repeat
						end if
					end repeat
				end try
			end repeat
		end tell
	end try
	return n
end tell
