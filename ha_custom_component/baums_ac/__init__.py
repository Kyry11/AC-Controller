"""The My Custom Air Conditioner integration."""
import logging

import aiohttp
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, Platform
from homeassistant.core import HomeAssistant

from .api import MyACApiClient
from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)
PLATFORMS = [Platform.CLIMATE, Platform.SWITCH]

async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up My AC from a config entry."""
    hass.data.setdefault(DOMAIN, {})

    # Create an aiohttp session and your API client
    session = aiohttp.ClientSession()
    api_client = MyACApiClient(host=entry.data[CONF_HOST], session=session)

    # Store the API client in hass.data for your platforms to access
    hass.data[DOMAIN][entry.entry_id] = api_client

    # Forward the setup to the climate and switch platforms
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    # Start the WebSocket listener
    # You will need to implement this part in your api.py
    # hass.async_create_task(api_client.start_websocket_listener())

    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    # Stop the WebSocket listener
    api_client = hass.data[DOMAIN][entry.entry_id]
    # await api_client.stop_websocket_listener()

    # Unload platforms
    unload_ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unload_ok:
        # Close the client session and remove data
        await api_client.session.close()
        hass.data[DOMAIN].pop(entry.entry_id)

    return unload_ok