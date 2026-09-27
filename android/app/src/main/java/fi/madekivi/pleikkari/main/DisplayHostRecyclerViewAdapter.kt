// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.main

import android.view.LayoutInflater
import android.view.ViewGroup
import android.widget.PopupMenu
import androidx.core.view.isGone
import androidx.core.view.isVisible
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.RecyclerView
import fi.madekivi.pleikkari.R
import fi.madekivi.pleikkari.common.DiscoveredDisplayHost
import fi.madekivi.pleikkari.databinding.ItemConsoleListHeaderBinding
import fi.madekivi.pleikkari.databinding.ItemDisplayHostBinding
import fi.madekivi.pleikkari.remote.ConnectProgress
import fi.madekivi.pleikkari.remote.applyTo
import fi.madekivi.pleikkari.remote.hideConnectBar

/**
 * A row in the console list (PLE-338): either the "Network discovered PlayStation(s)"
 * heading, shown once above the consoles that are found on the network but not linked
 * yet, or a console card.
 */
private sealed class ConsoleListRow
{
	data class Header(val unlinkedDiscoveredCount: Int): ConsoleListRow()
	data class Console(val console: HomeConsole): ConsoleListRow()
}

/** True for a console found on the network that has not been linked yet (PLE-338). */
private val HomeConsole.isUnlinkedDiscovered: Boolean
	get() = status == HomeConsoleStatus.REGISTRATION_REQUIRED && displayHost is DiscoveredDisplayHost

/**
 * Inserts the discovered-consoles heading directly above the first unlinked, network
 * discovered console. [consoles] is already sorted with unlinked consoles grouped
 * together (see [mergeHomeConsoles]), so this reads as one heading over one group.
 */
private fun buildRows(consoles: List<HomeConsole>): List<ConsoleListRow>
{
	val unlinkedDiscoveredCount = consoles.count(HomeConsole::isUnlinkedDiscovered)
	if(unlinkedDiscoveredCount == 0)
		return consoles.map(ConsoleListRow::Console)
	val headerIndex = consoles.indexOfFirst(HomeConsole::isUnlinkedDiscovered)
	return buildList {
		consoles.forEachIndexed { index, console ->
			if(index == headerIndex)
				add(ConsoleListRow.Header(unlinkedDiscoveredCount))
			add(ConsoleListRow.Console(console))
		}
	}
}

private fun ConsoleListRow.rowKey(): String = when(this)
{
	is ConsoleListRow.Header -> "header"
	is ConsoleListRow.Console -> "console:${console.key}"
}

private class ConsoleListRowDiffCallback(
	private val old: List<ConsoleListRow>,
	private val new: List<ConsoleListRow>
): DiffUtil.Callback()
{
	override fun areItemsTheSame(oldItemPosition: Int, newItemPosition: Int) =
		old[oldItemPosition].rowKey() == new[newItemPosition].rowKey()
	override fun areContentsTheSame(oldItemPosition: Int, newItemPosition: Int) =
		old[oldItemPosition] == new[newItemPosition]
	override fun getOldListSize() = old.size
	override fun getNewListSize() = new.size
}

class DisplayHostRecyclerViewAdapter(
	private val play: (HomeConsole) -> Unit,
	private val wake: (HomeConsole) -> Unit,
	private val edit: (HomeConsole) -> Unit,
	private val delete: (HomeConsole) -> Unit
): RecyclerView.Adapter<RecyclerView.ViewHolder>()
{
	private var rows: List<ConsoleListRow> = emptyList()

	var consoles: List<HomeConsole> = emptyList()
		set(value)
		{
			val newRows = buildRows(value)
			val diff = DiffUtil.calculateDiff(ConsoleListRowDiffCallback(rows, newRows))
			field = value
			rows = newRows
			diff.dispatchUpdatesTo(this)
		}

	var action: PsnConsoleActionState? = null
		set(value)
		{
			field = value
			notifyDataSetChanged()
		}

	/** Live progress for the console named by [action] (PLE-337); null when nothing is connecting. */
	var progress: ConnectProgress? = null
		set(value)
		{
			if(field == value)
				return
			field = value
			notifyDataSetChanged()
		}

	class HeaderViewHolder(val binding: ItemConsoleListHeaderBinding): RecyclerView.ViewHolder(binding.root)
	class ConsoleViewHolder(val binding: ItemDisplayHostBinding): RecyclerView.ViewHolder(binding.root)

	override fun getItemViewType(position: Int) = when(rows[position])
	{
		is ConsoleListRow.Header -> VIEW_TYPE_HEADER
		is ConsoleListRow.Console -> VIEW_TYPE_CONSOLE
	}

	override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) = when(viewType)
	{
		VIEW_TYPE_HEADER -> HeaderViewHolder(
			ItemConsoleListHeaderBinding.inflate(LayoutInflater.from(parent.context), parent, false)
		)
		else -> ConsoleViewHolder(
			ItemDisplayHostBinding.inflate(LayoutInflater.from(parent.context), parent, false)
		)
	}

	override fun getItemCount() = rows.size

	override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int)
	{
		when(val row = rows[position])
		{
			is ConsoleListRow.Header -> bindHeader(holder as HeaderViewHolder, row)
			is ConsoleListRow.Console -> bindConsole(holder as ConsoleViewHolder, row.console)
		}
	}

	private fun bindHeader(holder: HeaderViewHolder, header: ConsoleListRow.Header)
	{
		val resources = holder.itemView.resources
		holder.binding.root.text = resources.getQuantityString(
			R.plurals.console_list_discovered_heading,
			header.unlinkedDiscoveredCount,
			header.unlinkedDiscoveredCount
		)
	}

	private fun bindConsole(holder: ConsoleViewHolder, console: HomeConsole)
	{
		val context = holder.itemView.context
		val thisBusy = action?.duid?.let { it == console.psnConsole?.device?.duid } == true
		val anyBusy = action != null
		val linked = console.status != HomeConsoleStatus.REGISTRATION_REQUIRED
		holder.binding.apply {
			nameTextView.text = console.name
			// PLE-338: the "Not linked yet" status label duplicated the heading and the
			// Link button once both named the unlinked state, so it is dropped here; the
			// other statuses (On/Standby/Remote) still carry information the button and
			// tag do not, so they stay.
			// PLE-291: an unlinked console in rest mode still says so, since the button
			// wakes it as well as linking it.
			val resting = console.isUnlinkedResting
			statusTextView.isVisible = linked || resting
			statusTextView.setText(when(console.status)
			{
				HomeConsoleStatus.ON -> R.string.console_status_on
				HomeConsoleStatus.STANDBY -> R.string.console_status_standby
				HomeConsoleStatus.REMOTE -> if(console.psnConsole != null)
					R.string.console_status_remote_psn else R.string.console_status_remote
				HomeConsoleStatus.REGISTRATION_REQUIRED -> if(resting)
					R.string.console_status_standby else R.string.console_status_registration_required
			})
			val consoleProgress = if(thisBusy) progress else null
			detailTextView.text = console.detail
			detailTextView.isVisible = !console.detail.isNullOrBlank() && consoleProgress == null
			// The tag describes the resting state and the progress rows describe an
			// in-flight connect; PLE-337's progress and PLE-338's tag never show together.
			linkedTagTextView.isVisible = linked && consoleProgress == null
			actionStatusTextView.isVisible = consoleProgress != null
			if(consoleProgress != null)
			{
				actionStatusTextView.setText(consoleProgress.phase.labelRes)
				consoleProgress.applyTo(actionConnectProgressBar, actionOvertimeTextView)
			}
			else
				hideConnectBar(actionConnectProgressBar, actionOvertimeTextView)
			stateIndicatorImageView.setImageResource(
				if(console.displayHost?.isPS5 != false) R.drawable.ic_console_ps5 else R.drawable.ic_console
			)
			actionProgressBar.isVisible = thisBusy
			playButton.setText(when
			{
				linked -> R.string.action_connect
				resting -> R.string.action_wake_and_link
				else -> R.string.action_link
			})
			playButton.isEnabled = !anyBusy
			playButton.setOnClickListener { play(console) }
			wakeButton.isVisible = console.status == HomeConsoleStatus.STANDBY
			wakeButton.isEnabled = !anyBusy
			wakeButton.setOnClickListener { wake(console) }

			val editable = console.manualDisplayHost != null
			menuButton.isVisible = editable
			if(editable)
				menuButton.setOnClickListener {
					PopupMenu(context, menuButton).also { menu ->
						menu.menuInflater.inflate(R.menu.display_host, menu.menu)
						menu.menu.findItem(R.id.action_wakeup).isVisible = false
						menu.setOnMenuItemClickListener { item ->
							when(item.itemId)
							{
								R.id.action_edit -> edit(console)
								R.id.action_delete -> delete(console)
								else -> return@setOnMenuItemClickListener false
							}
							true
						}
						menu.show()
					}
				}
			else
				menuButton.setOnClickListener(null)
			menuButton.isGone = !editable
		}
	}

	private companion object
	{
		const val VIEW_TYPE_HEADER = 0
		const val VIEW_TYPE_CONSOLE = 1
	}
}
