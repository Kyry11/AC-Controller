"""Climate platform for My Custom Air Conditioner."""
import logging
from homeassistant.components.climate import (
    ClimateEntity,
    ClimateEntityFeature,
    HVACMode,
    FAN_AUTO, FAN_LOW, FAN_MEDIUM, FAN_HIGH,
)
from homeassistant.const import UnitOfTemperature, ATTR_TEMPERATURE
from homeassistant.core import callback

from .api import MyACApiClient
from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)

async def async_setup_entry(hass, entry, async_add_entities):
    """Set up the My AC climate platform."""
    api_client: MyACApiClient = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([MyACClimateEntity(api_client, entry.title)])

class MyACClimateEntity(ClimateEntity):
    """Representation of the main AC unit."""
    _attr_has_entity_name = True
    _attr_name = None  # We use the device name as the entity name

    # Define supported features, modes, and units
    _attr_supported_features = (
        ClimateEntityFeature.TARGET_TEMPERATURE
        | ClimateEntityFeature.FAN_MODE
        | ClimateEntityFeature.TURN_ON
        | ClimateEntityFeature.TURN_OFF
    )
    _attr_hvac_modes = [HVACMode.COOL, HVACMode.HEAT, HVACMode.FAN_ONLY, HVACMode.OFF]
    _attr_fan_modes = [FAN_AUTO, FAN_LOW, FAN_MEDIUM, FAN_HIGH]
    _attr_temperature_unit = UnitOfTemperature.CELSIUS

    def __init__(self, api_client: MyACApiClient, device_name: str):
        """Initialize the climate entity."""
        self._api = api_client
        self._attr_unique_id = f"{DOMAIN}_climate"
        self._attr_device_info = {
            "identifiers": {(DOMAIN, "main_ac_unit")},
            "name": device_name,
            "manufacturer": "My Custom Build",
        }

    @property
    def hvac_mode(self) -> HVACMode | None:
        """Return current HVAC mode."""
        power = self._api.state.get("power", "off")
        if power == "off":
            return HVACMode.OFF
        return self._api.state.get("mode")

    @property
    def current_temperature(self) -> float | None:
        """Return the current temperature."""
        return self._api.state.get("current_temp")

    @property
    def target_temperature(self) -> float | None:
        """Return the temperature we try to reach."""
        return self._api.state.get("target_temp")

    @property
    def fan_mode(self) -> str | None:
        """Return the fan setting."""
        return self._api.state.get("fan_speed")

    async def async_set_hvac_mode(self, hvac_mode: HVACMode):
        """Set new target hvac mode."""
        if hvac_mode == HVACMode.OFF:
            await self._api.set_power(False)
        else:
            await self._api.set_power(True)
            await self._api.set_hvac_mode(hvac_mode)
        await self.async_update_ha_state(True)

    async def async_set_temperature(self, **kwargs):
        """Set new target temperature."""
        if (temperature := kwargs.get(ATTR_TEMPERATURE)) is not None:
            await self._api.set_temperature(temperature)
            await self.async_update_ha_state(True)

    async def async_set_fan_mode(self, fan_mode: str):
        """Set new target fan mode."""
        await self._api.set_fan_mode(fan_mode)
        await self.async_update_ha_state(True)

    async def async_turn_on(self):
        """Turn the entity on."""
        await self.async_set_hvac_mode(HVACMode.COOL) # Default to cool

    async def async_turn_off(self):
        """Turn the entity off."""
        await self.async_set_hvac_mode(HVACMode.OFF)

    async def async_update(self):
        """
        Update the entity.
        This is called periodically by Home Assistant.
        For a WebSocket implementation, this would be less important.
        """
        _LOGGER.debug("Updating climate entity state")
        await self._api.get_state()