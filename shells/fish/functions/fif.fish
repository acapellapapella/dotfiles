# find file by name
function fif
	if test -z "$argv[1]"
		echo "Usage: fif <pattern>"
		return 1
	end
	find . -name "*$argv[1]*"
end
