"""Switch platform for My Custom Air Conditioner zones."""
import logging
from homeassistant.components.switch import SwitchEntity
from homeassistant.core import callback

from .api import MyACApiClient
from .const import DOMAIN, ZONES

_LOGGER = logging.getLogger(__name__)

async def async_setup_entry(hass, entry, async_add_entities):
    """Set up the My AC switch platform."""
    api_client: MyACApiClient = hass.data[DOMAIN][entry.entry_id]

    # Create a switch for each zone defined in const.py
    switches = [MyACZoneSwitch(api_client, zone) for zone in ZONES]
    async_add_entities(switches)

class MyACZoneSwitch(SwitchEntity):
    """Representation of an AC zone damper switch."""
    _attr_has_entity_name = True

    def __init__(self, api_client: MyACApiClient, zone_name: str):
        """Initialize the switch."""
        self._api = api_client
        self._zone_name = zone_name
        self._attr_name = f"Zone {zone_name}"
        self._attr_unique_id = f"{DOMAIN}_zone_{zone_name.lower().replace(' ', '_')}"

        # Link this switch to the main AC device
        self._attr_device_info = {
            "identifiers": {(DOMAIN, "main_ac_unit")},
        }

    @property
    def is_on(self) -> bool | None:
        """Return true if the switch is on (damper is open)."""
        zones = self._api.state.get("zones", {})
        # Assumes your API returns 'open' for an on state
        return zones.get(self._zone_name) == "open"

    async def async_turn_on(self, **kwargs):
        """Turn the zone on (open damper)."""
        await self._api.set_damper_state(self._zone_name, "open")
        await self.async_update_ha_state(True)

    async def async_turn_off(self, **kwargs):
        """Turn the zone off (close damper)."""
        await self._api.set_damper_state(self._zone_name, "close")
        await self.async_update_ha_state(True)

    async def async_update(self):
        """
        Update the entity.
        The climate entity handles the main poll, so this is just to ensure
        the state is processed, but no new API call is needed here.
        """
        _LOGGER.debug("Updating switch entity state for zone: %s", self._zone_name)
        # The main `climate` entity's update call will refresh the data
        # for all entities, so we don't need to make a separate API call here.