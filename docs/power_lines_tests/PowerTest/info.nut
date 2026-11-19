class PowerTest extends GSInfo {
	function GetAuthor() { return "test"; }
	function GetName() { return "PowerTest"; }
	function GetDescription() { return "Tests the power script API"; }
	function GetVersion() { return 1; }
	function GetDate() { return "2026-11-19"; }
	function CreateInstance() { return "PowerTest"; }
	function GetShortName() { return "PWRT"; }
	function GetAPIVersion() { return "16"; }
}
RegisterGS(PowerTest());
