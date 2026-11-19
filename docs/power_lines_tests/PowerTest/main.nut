class PowerTest extends GSController {
	function Log(s) { GSLog.Info("PT " + s); }

	function FindSubstation(centre, r) {
		for (local d = 0; d <= r; d++) {
			for (local dy = -d; dy <= d; dy++) for (local dx = -d; dx <= d; dx++) {
				if (abs(dx) != d && abs(dy) != d) continue;
				local t = centre + GSMap.GetTileIndex(dx, dy) - GSMap.GetTileIndex(0, 0);
				if (!GSMap.IsValidTile(t)) continue;
				if (GSPower.BuildSubstation(t)) return t;
			}
		}
		return -1;
	}

	function Events() {
		while (GSEventController.IsEventWaiting()) {
			local e = GSEventController.GetNextEvent();
			switch (e.GetEventType()) {
				case GSEvent.ET_INDUSTRY_POWERED: {
					local ev = GSEventIndustryPowered.Convert(e);
					Log("EVENT industry powered " + GSIndustry.GetName(ev.GetIndustryID()) + " supplied " + GSIndustry.GetPowerSupplied(ev.GetIndustryID()));
					break;
				}
				case GSEvent.ET_POWER_NETWORK_SHORT: {
					local ev = GSEventPowerNetworkShort.Convert(e);
					Log("EVENT network short at " + GSMap.GetTileX(ev.GetTile()) + "," + GSMap.GetTileY(ev.GetTile()) + " supply " + ev.GetSupply() + " demand " + ev.GetDemand() + " received " + ev.GetReceived());
					break;
				}
			}
		}
	}

	function Start() {
		local wind = -1;
		foreach (t, _ in GSIndustryTypeList()) {
			if (GSIndustryType.GetName(t).find("Wind") != null) wind = t;
		}
		Log("wind type " + wind + " max span " + GSPower.GetMaxSpan());
		while (GSCompany.ResolveCompanyID(0) == GSCompany.COMPANY_INVALID) GSController.Sleep(10);

		/* A consumer industry with room for a wind farm 12 tiles east. */
		local consumer = -1, farm = -1;
		foreach (i, _ in GSIndustryList()) {
			if (GSIndustry.IsPowerPlant(i)) continue;
			local loc = GSIndustry.GetLocation(i);
			for (local dx = 12; dx <= 20 && farm == -1; dx += 2) {
				for (local dy = -4; dy <= 4 && farm == -1; dy += 2) {
					local t = loc + GSMap.GetTileIndex(dx, dy) - GSMap.GetTileIndex(0, 0);
					if (GSMap.IsValidTile(t) && GSIndustryType.BuildIndustry(wind, t)) farm = t;
				}
			}
			if (farm != -1) { consumer = i; break; }
		}
		if (farm == -1) { Log("FAIL no wind farm"); return; }
		local farm_id = GSIndustry.GetIndustryID(farm);
		Log("consumer " + GSIndustry.GetName(consumer) + " plant " + GSIndustry.GetName(farm_id) + " is plant " + GSIndustry.IsPowerPlant(farm_id) + " demand " + GSIndustry.GetPowerDemand(consumer));

		/* A big town on a small wind farm, to see a network short of energy. */
		local tl = GSTownList(); tl.Valuate(GSTown.GetPopulation); tl.Sort(GSList.SORT_BY_VALUE, false);
		local big = tl.Begin();
		local centre = GSTown.GetLocation(big);
		Log("big town " + GSTown.GetName(big) + " pop " + GSTown.GetPopulation(big) + " demand " + GSTown.GetPowerDemand(big));
		local farm2 = -1;
		{
			for (local dy = -24; dy <= 24 && farm2 == -1; dy += 2) {
				for (local dx = -24; dx <= 24 && farm2 == -1; dx += 2) {
					if (abs(dx) < 10 && abs(dy) < 10) continue;
					local x = GSMap.GetTileX(centre) + dx, y = GSMap.GetTileY(centre) + dy;
					if (x < 2 || y < 2 || x > GSMap.GetMapSizeX() - 8 || y > GSMap.GetMapSizeY() - 8) continue;
					local t = GSMap.GetTileIndex(x, y);
					if (GSIndustryType.BuildIndustry(wind, t)) farm2 = t;
				}
			}
			if (farm2 == -1) Log("farm2 err " + GSError.GetLastErrorString());
		}
		local cm = GSCompanyMode(0);
		local a = FindSubstation(farm, 3);
		local b = FindSubstation(GSIndustry.GetLocation(consumer), 3);
		Log("substations " + a + " " + b + " err " + GSError.GetLastErrorString());
		if (a == -1 || b == -1) return;
		Log("type a " + GSPower.GetPowerTileType(a) + " owner " + GSTile.GetOwner(a));
		/* An L of line: along x from a, then along y to b. */
		local corner = GSMap.GetTileIndex(GSMap.GetTileX(b), GSMap.GetTileY(a));
		local ok1 = GSPower.BuildLine(a, corner);
		Log("line 1 " + ok1 + " " + GSError.GetLastErrorString());
		local ok2 = GSPower.BuildLine(corner, b);
		Log("line 2 " + ok2 + " " + GSError.GetLastErrorString());
		Log("corner type " + GSPower.GetPowerTileType(corner) + " wire mid " + GSPower.HasWire(a + GSMap.GetTileIndex(1, 0) - GSMap.GetTileIndex(0, 0)));
		Log("bad line " + GSPower.BuildLine(a, a + GSMap.GetTileIndex(3, 1) - GSMap.GetTileIndex(0, 0)) + " " + GSError.GetLastErrorString());

		for (local n = 0; n < 12; n++) {
			GSController.Sleep(300);
			Events();
			Log("step " + n + " a status " + GSPower.GetSubstationStatus(a) + " sent " + GSPower.GetSubstationSent(a) + " cap " + GSPower.GetSubstationCapacity(a)
				+ " | b received " + GSPower.GetSubstationReceived(b) + " | net supply " + GSPower.GetNetworkSupply(b) + " demand " + GSPower.GetNetworkDemand(b) + " received " + GSPower.GetNetworkReceived(b)
				+ " | consumer on grid " + GSIndustry.IsOnPowerGrid(consumer) + " demand " + GSIndustry.GetPowerDemand(consumer) + " got " + GSIndustry.GetPowerSupplied(consumer)
				+ " | plant sold " + GSIndustry.GetPowerSupplied(farm_id));
		}
		Log("infra pieces " + GSInfrastructure.GetInfrastructurePieceCount(0, GSInfrastructure.INFRASTRUCTURE_POWER) + " cost " + GSInfrastructure.GetMonthlyInfrastructureCosts(0, GSInfrastructure.INFRASTRUCTURE_POWER));
		Log("sharing " + GSPower.IsSharing(0) + " set " + GSPower.SetSharing(true) + " now " + GSPower.IsSharing(0));
		local town = GSTile.GetClosestTown(b);
		Log("town " + GSTown.GetName(town) + " demand " + GSTown.GetPowerDemand(town) + " got " + GSTown.GetPowerSupplied(town));
		Log("cable " + GSPower.BuildCable(b + GSMap.GetTileIndex(0, 1) - GSMap.GetTileIndex(0, 0), b + GSMap.GetTileIndex(0, 4) - GSMap.GetTileIndex(0, 0)) + " " + GSError.GetLastErrorString());
		Log("remove corner " + GSPower.RemovePowerTile(corner) + " " + GSError.GetLastErrorString());
		GSController.Sleep(300);
		Events();
		Log("after removal a status " + GSPower.GetSubstationStatus(a) + " consumer got " + GSIndustry.GetPowerSupplied(consumer));
		Log("farm2 " + farm2);
		local c = FindSubstation(farm2, 3);
		local d = FindSubstation(centre, 3);
		local corner2 = GSMap.GetTileIndex(GSMap.GetTileX(d), GSMap.GetTileY(c));
		local l1 = GSPower.BuildLine(c, corner2);
		local l2 = l1 && GSPower.BuildLine(corner2, d);
		Log("subs " + c + " " + d + " lines " + l1 + " " + l2 + " " + GSError.GetLastErrorString());
		if (!l2) {
			if (l1) GSPower.RemoveLine(c, corner2);
			corner2 = GSMap.GetTileIndex(GSMap.GetTileX(c), GSMap.GetTileY(d));
			l1 = GSPower.BuildLine(c, corner2);
			l2 = l1 && GSPower.BuildLine(corner2, d);
			Log("other corner lines " + l1 + " " + l2 + " " + GSError.GetLastErrorString());
		}
		{
			local x0 = GSMap.GetTileX(c), y0 = GSMap.GetTileY(c), x1 = GSMap.GetTileX(d), y1 = GSMap.GetTileY(d), cx = GSMap.GetTileX(corner2), cy = GSMap.GetTileY(corner2);
			local str = "route c " + x0 + "," + y0 + " corner " + cx + "," + cy + " d " + x1 + "," + y1 + ": ";
			local walk = function(xa, ya, xb, yb) {
				local out = "";
				local sx = xb > xa ? 1 : (xb < xa ? -1 : 0), sy = yb > ya ? 1 : (yb < ya ? -1 : 0);
				local x = xa, y = ya;
				while (true) {
					local t = GSMap.GetTileIndex(x, y);
					out += GSPower.IsPowerTile(t) ? ("P" + GSPower.GetPowerTileType(t)) : (GSPower.HasWire(t) ? "-" : ".");
					if (x == xb && y == yb) break;
					x += sx; y += sy;
				}
				return out;
			};
			Log(str + walk(x0, y0, cx, cy) + " | " + walk(cx, cy, x1, y1));
		}
		for (local n = 0; n < 6; n++) {
			GSController.Sleep(300);
			Events();
			Log("big step " + n + " net supply " + GSPower.GetNetworkSupply(d) + " demand " + GSPower.GetNetworkDemand(d) + " received " + GSPower.GetNetworkReceived(d) + " town got " + GSTown.GetPowerSupplied(big)
				+ " | c status " + GSPower.GetSubstationStatus(c) + " c supply " + GSPower.GetNetworkSupply(c) + " farm2 on grid " + GSIndustry.IsOnPowerGrid(GSIndustry.GetIndustryID(farm2)) + " offer " + GSIndustry.GetPowerDemand(GSIndustry.GetIndustryID(farm2)) + " " + GSIndustry.GetName(GSIndustry.GetIndustryID(farm2)));
		}
		Log("DONE");
		while (true) { GSController.Sleep(100); Events(); }
	}
}
