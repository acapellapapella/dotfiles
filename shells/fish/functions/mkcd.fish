# create and go to the created directory
function mkcd
	if test -z "$argv[1]"
		echo "Usage: mkcd <pattern>"
		return 1
	end
    mkdir -p $argv[1] && cd $argv[1]
end
