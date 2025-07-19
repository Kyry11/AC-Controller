"""Config flow for My Custom Air Conditioner."""
import voluptuous as vol
from homeassistant import config_entries
from homeassistant.const import CONF_HOST

from .const import DOMAIN

class MyACConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Handle a config flow for My AC."""

    VERSION = 1

    async def async_step_user(self, user_input=None):
        """Handle the initial step."""
        errors = {}
        if user_input is not None:
            # Here you can add validation to test the connection to the API host
            # For now, we'll just accept any input.
            # Example: await test_connection(user_input[CONF_HOST])
            return self.async_create_entry(
                title=f"My AC ({user_input[CONF_HOST]})",
                data=user_input
            )

        data_schema = vol.Schema({
            vol.Required(CONF_HOST): str,
        })

        return self.async_show_form(
            step_id="user",
            data_schema=data_schema,
            errors=errors,
        )