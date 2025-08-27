import re
import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
from matplotlib.figure import Figure
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext
import numpy as np
from matplotlib.widgets import Cursor
import matplotlib.dates as mdates

class OSLogParser:
    def __init__(self):
        self.pattern = re.compile(
            r'\[(\d+)m\[(\w+)\s+(\d+)\]\[0m\s*(?:\[([^\]]+)\]\s*)?(.*)'
        )
        
    def parse_log_file(self, file_path):
        logs = []
        line_number = 0
        
        try:
            with open(file_path, 'r', encoding='utf-8') as file:
                for line in file:
                    line_number += 1
                    parsed = self.parse_line(line, line_number)
                    if parsed:
                        logs.append(parsed)
        except Exception as e:
            raise Exception(f"Error reading file: {str(e)}")
        
        return pd.DataFrame(logs) if logs else pd.DataFrame()
    
    def parse_line(self, line, line_number):
        match = self.pattern.match(line.strip())
        if not match:
            return None
            
        color_code, level, log_number, module, message = match.groups()
        
        level_color = {
            '32': 'INFO',
            '36': 'DEBUG', 
            '31': 'ERROR',
            '33': 'WARNING',
            '35': 'CRITICAL'
        }.get(color_code, 'UNKNOWN')
        
        # Extract tick timestamp from log number or message
        tick_timestamp = self.extract_tick_timestamp(log_number, message)
        
        return {
            'line_number': line_number,
            'tick_timestamp': tick_timestamp,
            'level': level_color,
            'log_number': int(log_number),
            'module': module if module else 'SYSTEM',
            'message': message.strip(),
            'raw_line': line.strip()
        }
    
    def extract_tick_timestamp(self, log_number, message):
        # Use log number as the primary timestamp (sequential)
        # Also look for explicit tick values in the message
        try:
            tick_value = int(log_number)  # Use log number as relative time
            
            # Look for explicit tick values in message
            tick_patterns = [
                r'tick[:\s]*(\d+)',
                r'time[:\s]*(\d+)',
                r'timestamp[:\s]*(\d+)'
            ]
            
            for pattern in tick_patterns:
                match = re.search(pattern, message, re.IGNORECASE)
                if match:
                    return int(match.group(1))
                    
            return tick_value
            
        except ValueError:
            return None

class LogAnalyzer:
    def __init__(self, df):
        self.df = df
        if not df.empty and 'tick_timestamp' in df.columns:
            # Normalize tick timestamps for better plotting
            if not df['tick_timestamp'].isna().all():
                min_tick = df['tick_timestamp'].min()
                self.df['relative_time'] = df['tick_timestamp'] - min_tick
                self.df['normalized_time'] = self.df['relative_time'] / max(1, self.df['relative_time'].max())
        
    def get_stats(self):
        if self.df.empty:
            return {}
            
        stats = {
            'total_entries': len(self.df),
            'error_count': len(self.df[self.df['level'] == 'ERROR']),
            'warning_count': len(self.df[self.df['level'] == 'WARNING']),
            'info_count': len(self.df[self.df['level'] == 'INFO']),
            'debug_count': len(self.df[self.df['level'] == 'DEBUG']),
            'modules': self.df['module'].nunique(),
        }
        
        # Add time stats if available
        if 'tick_timestamp' in self.df.columns and not self.df['tick_timestamp'].isna().all():
            stats['time_range'] = (self.df['tick_timestamp'].min(), self.df['tick_timestamp'].max())
            stats['time_span'] = stats['time_range'][1] - stats['time_range'][0]
        
        # Memory usage stats
        memory_stats = self.extract_memory_stats()
        stats.update(memory_stats)
        
        return stats
    
    def extract_memory_stats(self):
        stats = {}
        # Extract heap usage
        heap_lines = self.df[self.df['message'].str.contains('bytes used from heap', na=False)]
        if not heap_lines.empty:
            usage_matches = heap_lines['message'].str.extract(r'(\d+)/\d+ bytes used from heap')
            if not usage_matches.empty:
                stats['max_heap_usage'] = usage_matches.astype(float).max().iloc[0]
                stats['heap_size'] = 4096
                stats['heap_usage_pct'] = (stats['max_heap_usage'] / stats['heap_size']) * 100
        
        # Extract stack usage
        stack_lines = self.df[self.df['message'].str.contains('bytes used from stack', na=False)]
        if not stack_lines.empty:
            usage_matches = stack_lines['message'].str.extract(r'(\d+)/\d+ bytes used from stack')
            if not usage_matches.empty:
                stats['max_stack_usage'] = usage_matches.astype(float).max().iloc[0]
                stats['stack_size'] = 94152
                stats['stack_usage_pct'] = (stats['max_stack_usage'] / stats['stack_size']) * 100
            
        return stats

class LogAnalyzerApp:
    def __init__(self, root):
        self.root = root
        self.root.title("VAIOS Log Analyzer")
        self.root.geometry("1400x900")
        self.root.configure(bg='#f0f0f0')
        
        self.df = pd.DataFrame()
        self.parser = OSLogParser()
        self.analyzer = None
        self.current_figure = None
        self.current_canvas = None
        
        self.setup_ui()
        
    def setup_ui(self):
        # Create main paned window for resizable sections
        main_frame = ttk.Frame(self.root)
        main_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        # Top section - Controls and stats (remain same)
        top_frame = ttk.Frame(main_frame)
        top_frame.pack(fill=tk.X, pady=5)
        
        # File selection
        file_frame = ttk.Frame(top_frame)
        file_frame.pack(fill=tk.X, pady=5)
        
        ttk.Label(file_frame, text="Log File:").pack(side=tk.LEFT, padx=5)
        self.file_path = tk.StringVar()
        ttk.Entry(file_frame, textvariable=self.file_path, width=60).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        ttk.Button(file_frame, text="Browse", command=self.browse_file).pack(side=tk.LEFT, padx=5)
        ttk.Button(file_frame, text="Load", command=self.load_file).pack(side=tk.LEFT, padx=5)
        
        # Stats and controls frame
        stats_controls_frame = ttk.Frame(top_frame)
        stats_controls_frame.pack(fill=tk.BOTH, expand=True, pady=5)
        
        # Stats frame
        stats_frame = ttk.LabelFrame(stats_controls_frame, text="Statistics", padding="5")
        stats_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 5))
        
        self.stats_text = scrolledtext.ScrolledText(stats_frame, height=8, width=40)
        self.stats_text.pack(fill=tk.BOTH, expand=True)
        
        # Visualization controls
        viz_controls_frame = ttk.LabelFrame(stats_controls_frame, text="Visualizations", padding="5")
        viz_controls_frame.pack(side=tk.RIGHT, fill=tk.Y, padx=(5, 0))
        
        ttk.Button(viz_controls_frame, text="Level Distribution", command=self.plot_level_distribution, width=20).pack(pady=2)
        ttk.Button(viz_controls_frame, text="Memory Usage", command=self.plot_memory_usage, width=20).pack(pady=2)
        ttk.Button(viz_controls_frame, text="Module Activity", command=self.plot_module_activity, width=20).pack(pady=2)
        ttk.Button(viz_controls_frame, text="Event Timeline", command=self.plot_event_timeline, width=20).pack(pady=2)
        ttk.Button(viz_controls_frame, text="Error Analysis", command=self.plot_error_analysis, width=20).pack(pady=2)
        ttk.Button(viz_controls_frame, text="Clear Plot", command=self.clear_plot, width=20).pack(pady=2)
        
        h_paned = ttk.PanedWindow(main_frame, orient=tk.HORIZONTAL)
        h_paned.pack(fill=tk.BOTH, expand=True, pady=5)

        # Left: Plot area (3/4 width)
        plot_frame = ttk.LabelFrame(h_paned, text="Visualization", padding="5")
        h_paned.add(plot_frame, weight=3)
        self.plot_container = ttk.Frame(plot_frame)
        self.plot_container.pack(fill=tk.BOTH, expand=True)

        # Right: Log viewer (1/4 width)
        log_frame = ttk.LabelFrame(h_paned, text="Log Viewer", padding="5")
        h_paned.add(log_frame, weight=1)

        # Filter controls
        filter_frame = ttk.Frame(log_frame)
        filter_frame.pack(fill=tk.X, pady=5)
        ttk.Label(filter_frame, text="Filter by:").pack(side=tk.LEFT, padx=5)
        self.level_filter = tk.StringVar(value="ALL")
        ttk.Combobox(filter_frame, textvariable=self.level_filter, 
                    values=["ALL", "INFO", "DEBUG", "ERROR", "WARNING"], width=10).pack(side=tk.LEFT, padx=5)
        self.module_filter = tk.StringVar(value="ALL")
        self.module_combo = ttk.Combobox(filter_frame, textvariable=self.module_filter, width=15)
        self.module_combo.pack(side=tk.LEFT, padx=5)
        ttk.Button(filter_frame, text="Apply Filter", command=self.apply_filter).pack(side=tk.LEFT, padx=5)
        ttk.Button(filter_frame, text="Clear Filter", command=self.clear_filter).pack(side=tk.LEFT, padx=5)

        # Log text area
        self.log_text = scrolledtext.ScrolledText(log_frame, height=20)
        self.log_text.pack(fill=tk.BOTH, expand=True)
        
        self.root.update_idletasks()
        h_paned.sashpos(0, int(self.root.winfo_width() * 0.65)) 
    
    def browse_file(self):
        filename = filedialog.askopenfilename(
            title="Select Log File",
            filetypes=[("Log files", "*.log"), ("Text files", "*.txt"), ("All files", "*.*")]
        )
        if filename:
            self.file_path.set(filename)
            
    def load_file(self):
        if not self.file_path.get():
            messagebox.showerror("Error", "Please select a log file first")
            return
            
        try:
            self.df = self.parser.parse_log_file(self.file_path.get())
            if self.df.empty:
                messagebox.showwarning("Warning", "No valid log entries found in the file")
                return
                
            self.analyzer = LogAnalyzer(self.df)
            self.update_stats()
            self.update_log_view()
            self.update_module_filter()
            
        except Exception as e:
            messagebox.showerror("Error", f"Failed to load file: {str(e)}")
            
    def update_stats(self):
        if self.analyzer is None:
            return
            
        stats = self.analyzer.get_stats()
        self.stats_text.delete(1.0, tk.END)
        
        if stats:
            self.stats_text.insert(tk.END, f"Total Entries: {stats['total_entries']}\n")
            self.stats_text.insert(tk.END, f"INFO: {stats['info_count']}\n")
            self.stats_text.insert(tk.END, f"DEBUG: {stats['debug_count']}\n")
            self.stats_text.insert(tk.END, f"ERROR: {stats['error_count']}\n")
            self.stats_text.insert(tk.END, f"WARNING: {stats.get('warning_count', 0)}\n")
            self.stats_text.insert(tk.END, f"Unique Modules: {stats['modules']}\n")
            
            if 'time_range' in stats:
                self.stats_text.insert(tk.END, f"Time Range: {stats['time_range'][0]} - {stats['time_range'][1]} ticks\n")
                self.stats_text.insert(tk.END, f"Time Span: {stats['time_span']} ticks\n")
            
            if 'max_heap_usage' in stats:
                self.stats_text.insert(tk.END, f"Max Heap Usage: {stats['max_heap_usage']}/{stats['heap_size']} bytes ({stats['heap_usage_pct']:.1f}%)\n")
            if 'max_stack_usage' in stats:
                self.stats_text.insert(tk.END, f"Max Stack Usage: {stats['max_stack_usage']}/{stats['stack_size']} bytes ({stats['stack_usage_pct']:.1f}%)\n")
                
    def update_log_view(self):
        self.log_text.delete(1.0, tk.END)
        for _, row in self.df.iterrows():
            level_color = {
                'INFO': 'green',
                'DEBUG': 'blue',
                'ERROR': 'red',
                'WARNING': 'orange'
            }.get(row['level'], 'black')
            
            self.log_text.insert(tk.END, f"[{row['level']} {row['log_number']}] ", level_color)
            if row['module']:
                self.log_text.insert(tk.END, f"[{row['module']}] ", 'purple')
            self.log_text.insert(tk.END, f"{row['message']}\n", 'black')
            
        # Configure tags for colors
        for level, color in [('INFO', 'green'), ('DEBUG', 'blue'), ('ERROR', 'red'), ('WARNING', 'orange')]:
            self.log_text.tag_config(level, foreground=color)
        self.log_text.tag_config('purple', foreground='purple')
        
    def update_module_filter(self):
        if not self.df.empty:
            modules = ["ALL"] + sorted(self.df['module'].unique().tolist())
            self.module_filter.set("ALL")
            self.module_combo['values'] = modules
            
    def apply_filter(self):
        if self.df.empty:
            return
            
        filtered_df = self.df.copy()
        
        level_filter = self.level_filter.get()
        if level_filter != "ALL":
            filtered_df = filtered_df[filtered_df['level'] == level_filter]
            
        module_filter = self.module_filter.get()
        if module_filter != "ALL":
            filtered_df = filtered_df[filtered_df['module'] == module_filter]
            
        self.log_text.delete(1.0, tk.END)
        for _, row in filtered_df.iterrows():
            self.log_text.insert(tk.END, f"[{row['level']} {row['log_number']}] ", row['level'])
            if row['module']:
                self.log_text.insert(tk.END, f"[{row['module']}] ", 'purple')
            self.log_text.insert(tk.END, f"{row['message']}\n")
            
    def clear_filter(self):
        self.level_filter.set("ALL")
        self.module_filter.set("ALL")
        self.update_log_view()
        
    def clear_plot(self):
        """Clear the current plot"""
        for widget in self.plot_container.winfo_children():
            widget.destroy()
        self.current_figure = None
        self.current_canvas = None
        
    def create_interactive_plot(self, fig):
        """Create an interactive plot with toolbar"""
        self.clear_plot()
        
        # Create canvas and toolbar
        canvas = FigureCanvasTkAgg(fig, self.plot_container)
        toolbar = NavigationToolbar2Tk(canvas, self.plot_container)
        toolbar.update()
        
        # Pack canvas and toolbar
        canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        
        # Enable interactive features
        canvas.mpl_connect('motion_notify_event', self.on_hover)
        
        self.current_figure = fig
        self.current_canvas = canvas
        
        return canvas
        
    def on_hover(self, event):
        """Handle mouse hover events for tooltips"""
        if event.inaxes and self.current_figure:
            # You can add tooltip functionality here
            pass
        
    def plot_level_distribution(self):
        if self.analyzer is None or self.df.empty:
            return
            
        fig = Figure(figsize=(10, 6), dpi=100)
        ax = fig.add_subplot(111)
        
        level_counts = self.df['level'].value_counts()
        colors = ['green', 'blue', 'red', 'orange', 'purple']
        bars = level_counts.plot(kind='bar', ax=ax, color=colors[:len(level_counts)])
        
        # Add value labels on bars
        for i, v in enumerate(level_counts):
            ax.text(i, v + 0.1, str(v), ha='center', va='bottom')
        
        ax.set_title('Log Level Distribution', fontsize=14, fontweight='bold')
        ax.set_xlabel('Log Level')
        ax.set_ylabel('Count')
        ax.tick_params(axis='x', rotation=45)
        ax.grid(True, alpha=0.3)
        
        self.create_interactive_plot(fig)
        
    def plot_memory_usage(self):
        if self.analyzer is None or self.df.empty:
            return
            
        # Extract memory usage data
        heap_data = self.df[self.df['message'].str.contains('bytes used from heap', na=False)]
        stack_data = self.df[self.df['message'].str.contains('bytes used from stack', na=False)]
        
        if heap_data.empty and stack_data.empty:
            messagebox.showinfo("Info", "No memory usage data found in logs")
            return
            
        fig = Figure(figsize=(12, 8), dpi=100)
        
        if not heap_data.empty and not stack_data.empty:
            ax1 = fig.add_subplot(211)
            ax2 = fig.add_subplot(212)
        else:
            ax1 = fig.add_subplot(111)
            ax2 = None
            
        if not heap_data.empty:
            heap_usage = heap_data['message'].str.extract(r'(\d+)/\d+ bytes used from heap').astype(float)
            # Use relative time if available, otherwise use index
            if 'relative_time' in heap_data.columns:
                line1, = ax1.plot(heap_data['relative_time'], heap_usage, 'b-', label='Heap Usage', marker='o', markersize=3)
                ax1.set_xlabel('Relative Time (ticks)')
            else:
                line1, = ax1.plot(heap_data.index, heap_usage, 'b-', label='Heap Usage', marker='o', markersize=3)
                ax1.set_xlabel('Log Entry Index')
                
            ax1.set_ylabel('Heap Usage (bytes)')
            ax1.set_title('Memory Usage Over Time', fontsize=14, fontweight='bold')
            ax1.legend()
            ax1.grid(True, alpha=0.3)
            
        if not stack_data.empty:
            stack_usage = stack_data['message'].str.extract(r'(\d+)/\d+ bytes used from stack').astype(float)
            if ax2:
                if 'relative_time' in stack_data.columns:
                    line2, = ax2.plot(stack_data['relative_time'], stack_usage, 'r-', label='Stack Usage', marker='o', markersize=3)
                    ax2.set_xlabel('Relative Time (ticks)')
                else:
                    line2, = ax2.plot(stack_data.index, stack_usage, 'r-', label='Stack Usage', marker='o', markersize=3)
                    ax2.set_xlabel('Log Entry Index')
                ax2.set_ylabel('Stack Usage (bytes)')
                ax2.legend()
                ax2.grid(True, alpha=0.3)
            else:
                if 'relative_time' in stack_data.columns:
                    line2, = ax1.plot(stack_data['relative_time'], stack_usage, 'r-', label='Stack Usage', marker='o', markersize=3)
                else:
                    line2, = ax1.plot(stack_data.index, stack_usage, 'r-', label='Stack Usage', marker='o', markersize=3)
                ax1.set_ylabel('Usage (bytes)')
                ax1.legend()
                
        fig.tight_layout()
        self.create_interactive_plot(fig)
        
    def plot_module_activity(self):
        if self.analyzer is None or self.df.empty:
            return
            
        fig = Figure(figsize=(12, 6), dpi=100)
        ax = fig.add_subplot(111)
        
        module_counts = self.df['module'].value_counts()
        bars = module_counts.plot(kind='bar', ax=ax, color='skyblue')
        
        # Add value labels on bars
        for i, v in enumerate(module_counts):
            ax.text(i, v + 0.1, str(v), ha='center', va='bottom')
        
        ax.set_title('Activity by Module', fontsize=14, fontweight='bold')
        ax.set_xlabel('Module')
        ax.set_ylabel('Number of Log Entries')
        ax.tick_params(axis='x', rotation=45)
        ax.grid(True, alpha=0.3)
        
        self.create_interactive_plot(fig)
        
    def plot_event_timeline(self):
        if self.analyzer is None or self.df.empty or 'tick_timestamp' not in self.df.columns:
            messagebox.showinfo("Info", "No timestamp data available for timeline")
            return
            
        fig = Figure(figsize=(12, 8), dpi=100)
        ax = fig.add_subplot(111)
        
        # Create a scatter plot with different colors for each level
        colors = {'INFO': 'green', 'DEBUG': 'blue', 'ERROR': 'red', 'WARNING': 'orange'}
        
        for level in self.df['level'].unique():
            level_data = self.df[self.df['level'] == level]
            if not level_data.empty:
                ax.scatter(level_data['tick_timestamp'], 
                          [level] * len(level_data), 
                          label=level, 
                          alpha=0.7, 
                          s=50,
                          color=colors.get(level, 'gray'))
        
        ax.set_title('Event Timeline (Tick-based)', fontsize=14, fontweight='bold')
        ax.set_xlabel('Tick Timestamp')
        ax.set_ylabel('Log Level')
        ax.legend()
        ax.grid(True, alpha=0.3)
        
        self.create_interactive_plot(fig)
        
    def plot_error_analysis(self):
        if self.analyzer is None or self.df.empty:
            return
            
        # Count errors by module
        error_data = self.df[self.df['level'] == 'ERROR']
        if error_data.empty:
            messagebox.showinfo("Info", "No errors found in logs")
            return
            
        fig = Figure(figsize=(10, 6), dpi=100)
        ax = fig.add_subplot(111)
        
        error_counts = error_data['module'].value_counts()
        bars = error_counts.plot(kind='bar', ax=ax, color='red')
        
        # Add value labels on bars
        for i, v in enumerate(error_counts):
            ax.text(i, v + 0.1, str(v), ha='center', va='bottom')
        
        ax.set_title('Error Distribution by Module', fontsize=14, fontweight='bold')
        ax.set_xlabel('Module')
        ax.set_ylabel('Number of Errors')
        ax.tick_params(axis='x', rotation=45)
        ax.grid(True, alpha=0.3)
        
        self.create_interactive_plot(fig)

def main():
    root = tk.Tk()
    app = LogAnalyzerApp(root)
    root.mainloop()

if __name__ == "__main__":
    main()